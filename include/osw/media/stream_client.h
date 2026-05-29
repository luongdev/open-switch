/*
 * include/osw/media/stream_client.h
 *
 * osw::media::StreamClient — bidi gRPC client for MediaBridge::Stream.
 *
 * Threading model (W6 Track B):
 *   - Open() runs on the calling thread: constructs ClientContext, calls
 *     stub_->Stream(), sends StreamStart, reads StreamReady, spawns
 *     reader_thread_ and writer_thread_.
 *   - writer_thread_ drains the bounded send ring (capacity 256) and
 *     calls stream->Write() for each dequeued AudioFrame. Exits when
 *     the ring is closed AND empty, or when Write() returns false.
 *   - reader_thread_ loops stream->Read() and dispatches via
 *     StreamCallbacks.  On Read() returning false it calls Finish()
 *     and fires on_done.
 *   - Close() is idempotent: closes the ring, joins writer,
 *     calls stream->WritesDone(), joins reader.
 *
 * See FREESWITCH-FACTS FF-034 for gRPC bidi-stream lifetime semantics.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_STREAM_CLIENT_H_
#define OSW_MEDIA_STREAM_CLIENT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/grpcpp.h>

#include "open_switch/media/v1/media.grpc.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/media/audio_frame.h"

namespace osw::media {

/// Callbacks invoked from the reader thread.  Handlers must be fast or
/// hand off to a queue — do NOT block the reader thread.
/// Close() and SendAudio() are safe to call from within these callbacks.
struct StreamCallbacks {
    std::function<void(AudioFrame)> on_audio;
    std::function<void(open_switch::media::v1::Transcript)> on_transcript;
    std::function<void(open_switch::media::v1::AmdVerdict)> on_amd;
    std::function<void(open_switch::media::v1::Control)> on_control;
    /// Fired once when the reader thread exits, with the final gRPC status.
    std::function<void(grpc::Status)> on_done;
};

struct StreamConfig {
    std::string stream_id;  ///< optional module-owned stream id for logs
    std::string channel_uuid;
    std::string tenant_id;
    open_switch::media::v1::StreamStart::Purpose purpose;
    std::uint32_t sample_rate_hz;  ///< 8000 or 16000
    std::uint32_t channels = 1;
    open_switch::media::v1::AudioCodec codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    open_switch::media::v1::StreamStart::CallerSide side =
        open_switch::media::v1::StreamStart::CALLER_SIDE_UNSPECIFIED;
    std::string traceparent;    ///< optional W3C traceparent
    std::string start_message;  ///< optional TTS opening line
    std::map<std::string, std::string> variables;
    /// For server-output-only streams such as TTS playback, half-close the
    /// client write side after StreamStart/StreamReady. STT/voicebot keep it
    /// false because they continue sending caller audio/control upstream.
    bool half_close_writes_after_start = false;
    /// Bounded outgoing send ring capacity in frames. Default 256 is the
    /// legacy W6 size; recording relay overrides this from config ms.
    std::size_t send_ring_capacity_frames = 256;
};

class StreamClient {
  public:
    /// `channel` is the caller-owned gRPC channel (one shared per endpoint).
    StreamClient(std::shared_ptr<grpc::Channel> channel,
                 StreamConfig config,
                 StreamCallbacks callbacks) noexcept;

    /// Calls Close() if the stream is still open.
    ~StreamClient() noexcept;

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;
    StreamClient(StreamClient&&) = delete;
    StreamClient& operator=(StreamClient&&) = delete;

    /// Open the stream: sends StreamStart, blocks up to `open_deadline_ms`
    /// waiting for StreamReady, then spawns the reader and writer threads.
    grpc::Status Open(int open_deadline_ms = 5000) noexcept;

    /// Enqueue an AudioFrame into the bounded send ring (capacity 256).
    /// On overflow, drops the OLDEST frame and increments frames_dropped().
    /// Returns false when a frame was dropped or the payload is empty.
    bool SendAudio(AudioFrame frame) noexcept;

    /// Enqueue a Control message (passed through to the writer thread).
    void SendControl(open_switch::media::v1::Control msg) noexcept;

    /// Half-close the send side + join threads. Idempotent.
    grpc::Status Close() noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] std::uint64_t frames_sent() const noexcept;
    [[nodiscard]] std::uint64_t frames_dropped() const noexcept;

    /// W6.5 fix (Gemini-P1): per-stream seq + timestamp counters.
    /// Replaces the previous `static thread_local` state used in
    /// OswStreamingReadTap, which leaked state across calls because FS
    /// reuses media threads for multiple channels.  These atomics live
    /// on the StreamClient, so they're naturally per-stream / per-call.
    ///
    /// `NextSeq()` returns the next monotonic sequence number (0-based).
    /// `AdvanceTimestamp(samples)` returns the pre-increment timestamp
    /// (in samples since stream start) and bumps the cursor by `samples`.
    [[nodiscard]] std::uint64_t NextSeq() noexcept {
        return seq_counter_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t AdvanceTimestamp(std::uint32_t samples) noexcept {
        return ts_counter_.fetch_add(samples, std::memory_order_relaxed);
    }

  private:
    // ----------------------------------------------------------------
    // Send ring: bounded queue of outgoing AudioFrame protos.
    // Capacity defaults to 256 ≈ 5 seconds @ 20ms ptime; W7 recording
    // relay may set a smaller per-stream capacity from config.
    // ----------------------------------------------------------------
    struct RingEntry {
        open_switch::media::v1::FromModule msg;
    };

    // Ring state guarded by ring_mu_.
    std::mutex ring_mu_;
    std::condition_variable ring_cv_;
    std::vector<RingEntry> ring_;  ///< circular buffer; size is configured per stream
    std::size_t ring_head_ = 0;    ///< next-write index
    std::size_t ring_tail_ = 0;    ///< next-read index
    std::size_t ring_count_ = 0;   ///< items currently in the ring
    bool ring_closed_ = false;     ///< set by Close(); writer exits when empty+closed

    // Push an entry. If full, drops oldest (advances tail) and increments
    // frames_dropped_. Called with ring_mu_ held.
    void RingPushLocked(RingEntry entry) noexcept;
    // Pop an entry. Returns false when ring is empty.  Called with ring_mu_ held.
    bool RingPopLocked(RingEntry& out) noexcept;

    // ----------------------------------------------------------------
    // gRPC objects
    // ----------------------------------------------------------------
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<open_switch::media::v1::MediaBridge::Stub> stub_;

    // context_ MUST outlive stream_ per FF-034.
    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReaderWriter<open_switch::media::v1::FromModule,
                                             open_switch::media::v1::FromService>>
        stream_;

    // ----------------------------------------------------------------
    // Configuration + callbacks
    // ----------------------------------------------------------------
    StreamConfig config_;
    StreamCallbacks callbacks_;

    // ----------------------------------------------------------------
    // Internal state
    // ----------------------------------------------------------------
    mutable std::mutex mu_;  ///< guards open_, final_status_
    bool open_ = false;
    bool writes_done_ = false;
    grpc::Status final_status_ = grpc::Status::OK;

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<bool> first_rx_audio_logged_{false};
    // W6.5 fix (Gemini-P1): per-stream monotonic seq + timestamp.
    std::atomic<std::uint64_t> seq_counter_{0};
    std::atomic<std::uint64_t> ts_counter_{0};

    std::chrono::steady_clock::time_point open_started_at_{};
    std::chrono::steady_clock::time_point ready_at_{};
    std::string server_stream_id_;

    // ----------------------------------------------------------------
    // Threads
    // ----------------------------------------------------------------
    std::thread reader_thread_;
    std::thread writer_thread_;

    void ReaderLoop() noexcept;
    void WriterLoop() noexcept;

    // Sample rate + channel count agreed after StreamReady (may differ
    // from what we requested if the server downgrades).
    std::uint32_t agreed_sample_rate_hz_ = 0;
    std::uint32_t agreed_channels_ = 1;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_STREAM_CLIENT_H_
