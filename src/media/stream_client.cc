/*
 * src/media/stream_client.cc
 *
 * Implementation of osw::media::StreamClient.
 *
 * Threading overview:
 *   - Open() (caller thread): constructs context + stream, sends StreamStart,
 *     blocks waiting for StreamReady, then spawns reader_thread_ +
 *     writer_thread_.
 *   - writer_thread_: drains the bounded ring buffer and calls
 *     stream_->Write(). Exits when ring is closed+empty OR Write() fails.
 *   - reader_thread_: loops stream_->Read(), dispatches callbacks, calls
 *     stream_->Finish() when Read() returns false, fires on_done.
 *   - Close() (any thread): closes the ring, joins writer, calls
 *     stream_->WritesDone(), joins reader. Idempotent (guarded by mu_).
 *
 * Lifetime contract (FF-034):
 *   stream_ MUST be destroyed before context_ (they are ordered that way
 *   as members; C++ destroys members in reverse-declaration order, but we
 *   explicitly reset stream_ before context_ in Close() to be safe).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/stream_client.h"

#include <algorithm>
#include <chrono>
#include <future>

#include "osw/observability/log.h"

namespace osw::media {

namespace {
constexpr const char* kSubsystem = "media.stream_client";

// W6.5 P1-006 fix: bound on how long Close() will wait for the peer to
// half-close after we send WritesDone before forcefully cancelling the
// stream via context_->TryCancel().
constexpr std::chrono::milliseconds kDrainTimeout{2000};

long long MillisBetween(std::chrono::steady_clock::time_point start,
                        std::chrono::steady_clock::time_point end) noexcept {
    if (start.time_since_epoch().count() == 0) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}
}  // namespace

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

StreamClient::StreamClient(std::shared_ptr<grpc::Channel> channel,
                           StreamConfig config,
                           StreamCallbacks callbacks) noexcept
    : channel_(std::move(channel)), config_(std::move(config)), callbacks_(std::move(callbacks)) {
    stub_ = open_switch::media::v1::MediaBridge::NewStub(channel_);
    ring_.resize(std::max<std::size_t>(1, config_.send_ring_capacity_frames));
}

StreamClient::~StreamClient() noexcept {
    Close();
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

grpc::Status StreamClient::Open(int open_deadline_ms) noexcept {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (open_) {
            return grpc::Status::OK;
        }
    }

    const auto open_started = std::chrono::steady_clock::now();
    open_started_at_ = open_started;
    ready_at_ = {};
    server_stream_id_.clear();

    osw::log::Info(kSubsystem,
                   "event=osw.media_stream.open.start stream_id=%s channel_uuid=%s "
                   "tenant_id=%s traceparent=%s purpose=%d requested_rate_hz=%u "
                   "channels=%u codec=%d open_deadline_ms=%d",
                   config_.stream_id.c_str(),
                   config_.channel_uuid.c_str(),
                   config_.tenant_id.c_str(),
                   config_.traceparent.c_str(),
                   static_cast<int>(config_.purpose),
                   config_.sample_rate_hz,
                   config_.channels,
                   static_cast<int>(config_.codec),
                   open_deadline_ms);

    // W6.5 P1-005 fix: do NOT set ClientContext::set_deadline on the
    // long-lived context.  The previous code set a 5-second deadline on
    // the shared context, which fired ~5 seconds after Open returned and
    // cancelled the long-lived stream, so every TTS/STT/voicebot session
    // died at the 5-second mark in production.
    //
    // Instead: enforce the handshake deadline via a watchdog future that
    // calls context_->TryCancel() if the StreamStart+StreamReady exchange
    // doesn't complete within open_deadline_ms.  Once the handshake
    // completes, we cancel the watchdog and the long-lived stream
    // continues without any deadline.
    context_ = std::make_unique<grpc::ClientContext>();

    stream_ = stub_->Stream(context_.get());
    if (!stream_) {
        context_.reset();
        return grpc::Status(grpc::StatusCode::INTERNAL, "stub->Stream returned null");
    }

    // Build StreamStart.
    open_switch::media::v1::FromModule start_msg;
    auto* ss = start_msg.mutable_start();
    ss->set_channel_uuid(config_.channel_uuid);
    ss->set_tenant_id(config_.tenant_id);
    ss->set_purpose(config_.purpose);
    ss->set_sample_rate_hz(config_.sample_rate_hz);
    ss->set_channels(config_.channels);
    ss->set_codec(config_.codec);
    ss->set_side(config_.side);
    ss->set_traceparent(config_.traceparent);
    ss->set_start_message(config_.start_message);
    for (const auto& [k, v] : config_.variables) {
        (*ss->mutable_variables())[k] = v;
    }

    // Run the handshake in a future so we can wait_for with the deadline.
    // Capture by reference so the future can update the local state.
    std::promise<grpc::Status> handshake_promise;
    auto handshake_future = handshake_promise.get_future();
    open_switch::media::v1::FromService resp;
    std::thread handshake_thread(
        [this, &start_msg, &resp, p = std::move(handshake_promise)]() mutable {
            if (!stream_->Write(start_msg)) {
                const grpc::Status s = stream_->Finish();
                p.set_value(s.ok() ? grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                                  "Write(StreamStart) failed")
                                   : s);
                return;
            }
            if (!stream_->Read(&resp)) {
                const grpc::Status s = stream_->Finish();
                p.set_value(
                    s.ok() ? grpc::Status(grpc::StatusCode::UNAVAILABLE, "no StreamReady received")
                           : s);
                return;
            }
            if (!resp.has_ready()) {
                p.set_value(grpc::Status(grpc::StatusCode::INTERNAL,
                                         "first server message was not StreamReady"));
                return;
            }
            if (config_.half_close_writes_after_start) {
                {
                    std::lock_guard<std::mutex> lock(ring_mu_);
                    ring_closed_ = true;
                }
                stream_->WritesDone();
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    writes_done_ = true;
                }
            }
            p.set_value(grpc::Status::OK);
        });

    grpc::Status handshake_status;
    if (handshake_future.wait_for(std::chrono::milliseconds(open_deadline_ms)) ==
        std::future_status::timeout) {
        // Force-cancel the stream so Write/Read return and handshake_thread can join.
        context_->TryCancel();
        // Drain the future — the handshake thread will see Write/Read return
        // false and resolve with CANCELLED (a side-effect of OUR TryCancel,
        // not the original cause).  Discard that side-effect status and
        // surface DEADLINE_EXCEEDED to the caller, matching the original
        // V1 contract that the test S2_OpenDeadlineExceeded asserts on.
        (void)handshake_future.get();
        handshake_status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                        "Open handshake exceeded open_deadline_ms");
    } else {
        handshake_status = handshake_future.get();
    }
    handshake_thread.join();

    if (!handshake_status.ok()) {
        stream_.reset();
        context_.reset();
        return handshake_status;
    }

    agreed_sample_rate_hz_ = resp.ready().sample_rate_hz();
    agreed_channels_ = resp.ready().channels();
    server_stream_id_ = resp.ready().server_stream_id();
    ready_at_ = std::chrono::steady_clock::now();

    // TODO(W6.4): add bounded latency histograms once media owns a
    // module-lifetime metrics helper with stable label cardinality.
    osw::log::Info(kSubsystem,
                   "event=osw.media_stream.ready stream_id=%s server_stream_id=%s "
                   "channel_uuid=%s tenant_id=%s traceparent=%s purpose=%d "
                   "agreed_rate_hz=%u channels=%u open_duration_ms=%lld",
                   config_.stream_id.c_str(),
                   server_stream_id_.c_str(),
                   config_.channel_uuid.c_str(),
                   config_.tenant_id.c_str(),
                   config_.traceparent.c_str(),
                   static_cast<int>(config_.purpose),
                   agreed_sample_rate_hz_,
                   agreed_channels_,
                   MillisBetween(open_started, ready_at_));

    // Mark open before spawning threads.
    {
        std::lock_guard<std::mutex> lock(mu_);
        open_ = true;
    }

    if (config_.half_close_writes_after_start) {
        reader_thread_ = std::thread(&StreamClient::ReaderLoop, this);
    } else {
        reader_thread_ = std::thread(&StreamClient::ReaderLoop, this);
        writer_thread_ = std::thread(&StreamClient::WriterLoop, this);
    }

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// SendAudio
// ---------------------------------------------------------------------------

bool StreamClient::SendAudio(AudioFrame frame) noexcept {
    if (frame.sample_count() == 0) {
        return false;
    }

    open_switch::media::v1::FromModule msg;
    frame.ToProto(msg.mutable_audio());

    RingEntry entry;
    entry.msg = std::move(msg);

    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(ring_mu_);
        if (ring_closed_) {
            return false;
        }
        if (ring_count_ == ring_.size()) {
            // Drop oldest: advance tail.
            ring_tail_ = (ring_tail_ + 1) % ring_.size();
            --ring_count_;
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            dropped = true;
        }
        ring_[ring_head_] = std::move(entry);
        ring_head_ = (ring_head_ + 1) % ring_.size();
        ++ring_count_;
    }
    ring_cv_.notify_one();
    return !dropped;
}

// ---------------------------------------------------------------------------
// SendControl
// ---------------------------------------------------------------------------

void StreamClient::SendControl(open_switch::media::v1::Control msg) noexcept {
    open_switch::media::v1::FromModule fmsg;
    *fmsg.mutable_control() = std::move(msg);

    RingEntry entry;
    entry.msg = std::move(fmsg);

    {
        std::lock_guard<std::mutex> lock(ring_mu_);
        if (ring_closed_) {
            return;
        }
        if (ring_count_ == ring_.size()) {
            ring_tail_ = (ring_tail_ + 1) % ring_.size();
            --ring_count_;
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        ring_[ring_head_] = std::move(entry);
        ring_head_ = (ring_head_ + 1) % ring_.size();
        ++ring_count_;
    }
    ring_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

grpc::Status StreamClient::Close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!open_) {
            return final_status_;
        }
        open_ = false;
    }

    // Signal writer to drain and exit (will exit once ring is closed+empty).
    {
        std::lock_guard<std::mutex> lock(ring_mu_);
        ring_closed_ = true;
    }
    ring_cv_.notify_all();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    // Half-close the send side: tell the server we are done sending.
    if (stream_) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!writes_done_) {
            stream_->WritesDone();
            writes_done_ = true;
        }
    }

    // W6.5 P1-006 fix: bounded drain on the reader thread.
    //
    // The previous code did `reader_thread_.join()` unbounded; if the peer
    // never half-closed in response to WritesDone(), the reader stayed
    // blocked on Read() forever and Close() (and module shutdown by
    // extension) hung indefinitely.  The codex review identified this as a
    // ship-stopping issue.
    //
    // Fix: spawn a tiny supervisor future that waits for the reader to
    // exit naturally within kDrainTimeout.  If the timeout elapses,
    // forcefully cancel the stream via context_->TryCancel(); Read() will
    // return false within a single network round-trip and the reader will
    // exit cleanly.  Only then do we join.
    //
    // Note: ReaderLoop owns the single Finish() call (line ~388).  Close()
    // intentionally does NOT call Finish() (per gRPC C++ docs, Finish must
    // be called exactly once).
    if (reader_thread_.joinable()) {
        auto reader_join = std::async(std::launch::async, [this]() { reader_thread_.join(); });
        if (reader_join.wait_for(kDrainTimeout) == std::future_status::timeout) {
            osw::log::Warn("media",
                           "StreamClient::Close: drain timeout (%lldms) — "
                           "TryCancel on context",
                           static_cast<long long>(kDrainTimeout.count()));
            if (context_) {
                context_->TryCancel();
            }
            reader_join.wait();  // now bounded — TryCancel unblocks Read()
        }
    }

    // Destroy stream before context (FF-034 lifetime contract).
    stream_.reset();
    context_.reset();

    std::lock_guard<std::mutex> lock(mu_);
    return final_status_;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool StreamClient::open() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return open_;
}

std::uint64_t StreamClient::frames_sent() const noexcept {
    return frames_sent_.load(std::memory_order_relaxed);
}

std::uint64_t StreamClient::frames_dropped() const noexcept {
    return frames_dropped_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Internal ring helpers
// ---------------------------------------------------------------------------

void StreamClient::RingPushLocked(RingEntry entry) noexcept {
    if (ring_count_ == ring_.size()) {
        ring_tail_ = (ring_tail_ + 1) % ring_.size();
        --ring_count_;
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    ring_[ring_head_] = std::move(entry);
    ring_head_ = (ring_head_ + 1) % ring_.size();
    ++ring_count_;
}

bool StreamClient::RingPopLocked(RingEntry& out) noexcept {
    if (ring_count_ == 0) {
        return false;
    }
    out = std::move(ring_[ring_tail_]);
    ring_tail_ = (ring_tail_ + 1) % ring_.size();
    --ring_count_;
    return true;
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

void StreamClient::WriterLoop() noexcept {
    for (;;) {
        RingEntry entry;
        {
            std::unique_lock<std::mutex> lock(ring_mu_);
            ring_cv_.wait(lock, [this] { return ring_count_ > 0 || ring_closed_; });
            if (!RingPopLocked(entry)) {
                // Ring is closed and empty: exit.
                break;
            }
        }

        if (!stream_->Write(entry.msg)) {
            // Write failed: stream broken or cancelled.
            osw::log::Warn(kSubsystem, "StreamClient::WriterLoop: Write() failed; exiting");
            break;
        }

        if (entry.msg.has_audio()) {
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------

void StreamClient::ReaderLoop() noexcept {
    open_switch::media::v1::FromService msg;

    while (stream_->Read(&msg)) {
        switch (msg.payload_case()) {
            case open_switch::media::v1::FromService::kAudio: {
                if (callbacks_.on_audio) {
                    auto frame = AudioFrame::FromProto(
                        msg.audio(), agreed_sample_rate_hz_, agreed_channels_);
                    if (frame.has_value()) {
                        bool expected = false;
                        if (first_rx_audio_logged_.compare_exchange_strong(
                                expected, true, std::memory_order_relaxed)) {
                            const auto first_audio_at = std::chrono::steady_clock::now();
                            osw::log::Info(
                                kSubsystem,
                                "event=osw.stream_client.first_audio.received "
                                "stream_id=%s server_stream_id=%s channel_uuid=%s "
                                "tenant_id=%s traceparent=%s purpose=%d seq=%llu "
                                "timestamp_samples=%llu duration_samples=%u "
                                "payload_bytes=%zu sample_count=%zu rate_hz=%u "
                                "channels=%u open_to_first_audio_ms=%lld "
                                "ready_to_first_audio_ms=%lld",
                                config_.stream_id.c_str(),
                                server_stream_id_.c_str(),
                                config_.channel_uuid.c_str(),
                                config_.tenant_id.c_str(),
                                config_.traceparent.c_str(),
                                static_cast<int>(config_.purpose),
                                static_cast<unsigned long long>(msg.audio().seq()),
                                static_cast<unsigned long long>(msg.audio().timestamp_samples()),
                                msg.audio().duration_samples(),
                                msg.audio().payload().size(),
                                frame->sample_count(),
                                frame->sample_rate_hz(),
                                frame->channels(),
                                MillisBetween(open_started_at_, first_audio_at),
                                MillisBetween(ready_at_, first_audio_at));
                        }
                        callbacks_.on_audio(std::move(*frame));
                    } else {
                        osw::log::Warn("media",
                                       "StreamClient::ReaderLoop: FromProto failed "
                                       "(payload size mismatch)");
                    }
                }
                break;
            }
            case open_switch::media::v1::FromService::kTranscript: {
                if (callbacks_.on_transcript) {
                    callbacks_.on_transcript(msg.transcript());
                }
                break;
            }
            case open_switch::media::v1::FromService::kAmd: {
                if (callbacks_.on_amd) {
                    callbacks_.on_amd(msg.amd());
                }
                break;
            }
            case open_switch::media::v1::FromService::kControl: {
                if (callbacks_.on_control) {
                    callbacks_.on_control(msg.control());
                }
                break;
            }
            case open_switch::media::v1::FromService::kReady:
                // Unexpected StreamReady after handshake; log and ignore.
                osw::log::Warn(kSubsystem,
                               "StreamClient::ReaderLoop: unexpected StreamReady mid-stream");
                break;
            case open_switch::media::v1::FromService::kVariables:
                // Variables message — not handled in V1; log and skip.
                osw::log::Debug(kSubsystem,
                                "StreamClient::ReaderLoop: Variables message (not handled in V1)");
                break;
            case open_switch::media::v1::FromService::PAYLOAD_NOT_SET:
            default:
                osw::log::Warn(kSubsystem,
                               "StreamClient::ReaderLoop: unknown payload_case %d",
                               static_cast<int>(msg.payload_case()));
                break;
        }
        msg.Clear();
    }

    // Read returned false: peer half-closed or cancelled.
    grpc::Status status = stream_->Finish();

    {
        std::lock_guard<std::mutex> lock(mu_);
        final_status_ = status;
    }

    if (status.error_message().empty()) {
        osw::log::Info(kSubsystem,
                       "StreamClient::ReaderLoop: stream finished; code=%d",
                       static_cast<int>(status.error_code()));
    } else {
        osw::log::Info(kSubsystem,
                       "StreamClient::ReaderLoop: stream finished; code=%d message=%s",
                       static_cast<int>(status.error_code()),
                       status.error_message().c_str());
    }

    if (callbacks_.on_done) {
        callbacks_.on_done(status);
    }
}

}  // namespace osw::media
