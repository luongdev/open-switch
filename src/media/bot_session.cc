/*
 * src/media/bot_session.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/bot_session.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/bug_fanout.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/media/stream_client.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"
#include "osw/raii/session_lock.h"

namespace osw::media {

namespace {
constexpr const char* kSubsystem = "media.bot_session";
constexpr std::uint32_t kDefaultPtimeMs = 20;
constexpr std::uint32_t kWriteReplaceFlag = static_cast<std::uint32_t>(SMBF_WRITE_REPLACE);
constexpr std::uint32_t kReadStreamFlag = static_cast<std::uint32_t>(SMBF_READ_STREAM);

using open_switch::control::v1::StartBotRequest;

open_switch::media::v1::StreamStart::Purpose MediaPurposeFor(
    StartBotRequest::Purpose purpose) noexcept {
    switch (purpose) {
        case StartBotRequest::TTS_BROADCAST:
            return open_switch::media::v1::StreamStart::TTS_PLAYBACK;
        case StartBotRequest::STT_LISTEN:
            return open_switch::media::v1::StreamStart::STT_TRANSCRIBE;
        case StartBotRequest::VOICEBOT_DUPLEX:
        case StartBotRequest::WHISPER:
            return open_switch::media::v1::StreamStart::VOICEBOT_DUPLEX;
        default:
            return open_switch::media::v1::StreamStart::PURPOSE_UNSPECIFIED;
    }
}

open_switch::media::v1::StreamStart::CallerSide MediaSideFor(
    StartBotRequest::Purpose purpose) noexcept {
    using StreamStart = open_switch::media::v1::StreamStart;
    switch (purpose) {
        case StartBotRequest::TTS_BROADCAST:
            return StreamStart::CALLER_EAR;
        case StartBotRequest::STT_LISTEN:
            return StreamStart::CALLER_MIC;
        case StartBotRequest::VOICEBOT_DUPLEX:
        case StartBotRequest::WHISPER:
            return StreamStart::BOTH_MIXED;
        default:
            return StreamStart::CALLER_SIDE_UNSPECIFIED;
    }
}

grpc::Status AttachFailureStatus(const MediaBugManager::AttachResult& attach) {
    grpc::StatusCode code = attach.status_code;
    if (code == grpc::StatusCode::ALREADY_EXISTS) {
        code = grpc::StatusCode::FAILED_PRECONDITION;
    }
    return grpc::Status(code, attach.error);
}

std::uint32_t SessionReadRate(switch_core_session_t* session) noexcept {
    switch_codec_implementation_t impl{};
    if (::osw::raii::fs::SessionGetReadImpl(session, &impl) != SWITCH_STATUS_SUCCESS) {
        return 0;
    }
    return impl.actual_samples_per_second != 0 ? impl.actual_samples_per_second
                                               : impl.samples_per_second;
}

std::uint32_t SessionWriteRate(switch_core_session_t* session) noexcept {
    switch_codec_implementation_t impl{};
    if (::osw::raii::fs::SessionGetWriteImpl(session, &impl) != SWITCH_STATUS_SUCCESS) {
        return 0;
    }
    return impl.actual_samples_per_second != 0 ? impl.actual_samples_per_second
                                               : impl.samples_per_second;
}

}  // namespace

BotSession::BotSession(BotSessionConfig cfg, std::shared_ptr<grpc::Channel> channel) noexcept
    : cfg_(std::move(cfg)), channel_(std::move(channel)) {
    BugFanout::Config fanout_cfg;
    fanout_cfg.mode = cfg_.purpose == StartBotRequest::WHISPER ? BugFanout::Mode::kWhisper
                                                               : BugFanout::Mode::kBroadcast;
    fanout_cfg.target_uuids = cfg_.target_channel_uuids;
    fanout_cfg.write_subset_uuids = cfg_.write_target_channel_uuids;
    fanout_cfg.capacity_frames = QueueCapacityFrames();
    fanout_ = std::make_unique<BugFanout>(std::move(fanout_cfg));
}

BotSession::~BotSession() noexcept {
    Stop();
}

grpc::Status BotSession::Open(int open_deadline_ms) noexcept {
    if (opened_.load(std::memory_order_acquire)) {
        return grpc::Status::OK;
    }
    if (!channel_) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "grpc channel unavailable");
    }
    if (cfg_.bot_id.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "bot_id required");
    }
    if (cfg_.target_channel_uuids.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "target_channel_uuids required");
    }

    StreamConfig sc;
    sc.stream_id = cfg_.bot_id;
    // The current MediaBridge proto has a single channel_uuid field. For the
    // TTS_BROADCAST slice, use the bot_id as the logical media stream id and
    // also pass target metadata through variables for upstream correlation.
    sc.channel_uuid = cfg_.bot_id;
    sc.tenant_id = cfg_.tenant_id;
    sc.purpose = MediaPurposeFor(cfg_.purpose);
    sc.sample_rate_hz = cfg_.sample_rate_hz;
    sc.channels = 1;
    sc.codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    sc.side = MediaSideFor(cfg_.purpose);
    sc.traceparent = cfg_.traceparent;
    sc.start_message = cfg_.start_message;
    sc.half_close_writes_after_start = !SupportsRead();
    sc.variables = cfg_.variables;
    sc.variables["osw.bot_id"] = cfg_.bot_id;
    for (std::size_t i = 0; i < cfg_.target_channel_uuids.size(); ++i) {
        sc.variables["osw.target." + std::to_string(i)] = cfg_.target_channel_uuids[i];
    }

    StreamCallbacks cbs;
    cbs.on_audio = [this](AudioFrame frame) noexcept {
        if (stopped_.load(std::memory_order_acquire) || !fanout_) {
            return;
        }
        const std::uint64_t dropped = fanout_->Push(std::move(frame));
        if (dropped > 0) {
            osw::audit::EmitSubclass("osw.media.bot.target_drop",
                                     {{"bot_id", cfg_.bot_id},
                                      {"tenant_id", cfg_.tenant_id},
                                      {"dropped_frames", std::to_string(dropped)}});
        }
        frames_recv_.fetch_add(1, std::memory_order_relaxed);
    };
    cbs.on_done = [this](grpc::Status status) noexcept {
        if (fanout_) {
            fanout_->HalfClose();
        }
        if (!status.ok()) {
            osw::log::Warn(kSubsystem,
                           "BotSession upstream done bot_id=%s code=%d message=%s",
                           cfg_.bot_id.c_str(),
                           static_cast<int>(status.error_code()),
                           status.error_message().c_str());
        }
    };

    client_ = std::make_unique<StreamClient>(channel_, std::move(sc), std::move(cbs));
    grpc::Status st = client_->Open(open_deadline_ms);
    if (!st.ok()) {
        client_.reset();
        return st;
    }
    opened_.store(true, std::memory_order_release);
    return grpc::Status::OK;
}

grpc::Status BotSession::Attach(MediaBugManager& mgr,
                                void* write_replace_callback,
                                void* read_tap_callback) noexcept {
    if (stopped_.load(std::memory_order_acquire)) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "bot session stopped");
    }
    if (SupportsWrite() && write_replace_callback == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "write callback required");
    }
    if (SupportsRead() && read_tap_callback == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read callback required");
    }

    auto unwind = [this](grpc::Status st) noexcept {
        handles_.clear();
        read_contexts_.clear();
        write_contexts_.clear();
        return st;
    };

    for (const auto& channel_uuid : cfg_.target_channel_uuids) {
        osw::SessionLock lock(channel_uuid.c_str());
        if (!lock) {
            return unwind(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found: " + channel_uuid));
        }

        if (SupportsRead()) {
            BugConfig read_cfg;
            read_cfg.purpose = Purpose::kVoicebotDuplexRead;
            if (cfg_.purpose == StartBotRequest::STT_LISTEN) {
                read_cfg.purpose = Purpose::kSttTranscribe;
            }
            read_cfg.fs_flags = kReadStreamFlag;
            read_cfg.target_rate_hz = cfg_.sample_rate_hz;
            read_cfg.tenant_id = cfg_.tenant_id;
            read_cfg.stream_endpoint = cfg_.upstream_endpoint;

            auto attach = mgr.Attach(lock.get(), std::move(read_cfg));
            if (!attach.ok) {
                return unwind(AttachFailureStatus(attach));
            }

            auto ctx = std::make_unique<BotReadTapCtx>();
            ctx->bot = this;
            ctx->channel_uuid = channel_uuid;
            ctx->stream_rate_hz = cfg_.sample_rate_hz;
            ctx->fs_rate_hz = SessionReadRate(lock.get());
            const std::uint64_t bug_id = MediaBugManager::BugId(attach.handle);
            if (!mgr.SetBugCallback(bug_id, read_tap_callback, ctx.get())) {
                return unwind(
                    grpc::Status(grpc::StatusCode::INTERNAL, "failed to wire read callback"));
            }
            handles_.push_back(std::move(attach.handle));
            read_contexts_.push_back(std::move(ctx));
        }

        if (ShouldAttachWrite(channel_uuid)) {
            BugConfig write_cfg;
            write_cfg.purpose = cfg_.purpose == StartBotRequest::VOICEBOT_DUPLEX
                                    ? Purpose::kVoicebotDuplexWrite
                                    : Purpose::kTtsPlayback;
            write_cfg.fs_flags = kWriteReplaceFlag;
            write_cfg.target_rate_hz = cfg_.sample_rate_hz;
            write_cfg.tenant_id = cfg_.tenant_id;
            write_cfg.stream_endpoint = cfg_.upstream_endpoint;

            auto attach = mgr.Attach(lock.get(), std::move(write_cfg));
            if (!attach.ok) {
                return unwind(AttachFailureStatus(attach));
            }

            auto ctx = std::make_unique<BotWriteReplaceCtx>();
            ctx->bot = this;
            ctx->channel_uuid = channel_uuid;
            ctx->fs_rate_hz = SessionWriteRate(lock.get());
            const std::uint64_t bug_id = MediaBugManager::BugId(attach.handle);
            if (!mgr.SetBugCallback(bug_id, write_replace_callback, ctx.get())) {
                return unwind(
                    grpc::Status(grpc::StatusCode::INTERNAL, "failed to wire write callback"));
            }
            handles_.push_back(std::move(attach.handle));
            write_contexts_.push_back(std::move(ctx));
        }
    }

    return grpc::Status::OK;
}

void BotSession::Stop() noexcept {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // Detach bugs before freeing callback contexts.
    handles_.clear();
    read_contexts_.clear();
    write_contexts_.clear();

    if (fanout_) {
        fanout_->HalfClose();
    }
    if (client_) {
        client_->Close();
        client_.reset();
    }
    opened_.store(false, std::memory_order_release);
}

void BotSession::OnTargetReadFrame(std::string_view channel_uuid,
                                   std::uint64_t /*fs_timestamp_samples*/,
                                   const std::int16_t* samples,
                                   std::size_t sample_count,
                                   std::uint32_t sample_rate_hz,
                                   std::uint32_t channels) noexcept {
    if (stopped_.load(std::memory_order_acquire) || !client_ || !samples || sample_count == 0) {
        return;
    }

    std::vector<std::int16_t> owned(samples, samples + sample_count);
    const std::uint32_t safe_channels = std::max<std::uint32_t>(channels, 1);
    const std::uint64_t seq = client_->NextSeq();
    const std::uint64_t ts =
        client_->AdvanceTimestamp(static_cast<std::uint32_t>(sample_count / safe_channels));
    AudioFrame frame(std::move(owned),
                     sample_rate_hz,
                     safe_channels,
                     seq,
                     ts,
                     /*channel=*/0,
                     std::string(channel_uuid));
    if (client_->SendAudio(std::move(frame))) {
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::optional<AudioFrame> BotSession::PopWriteFrame(std::string_view channel_uuid) noexcept {
    if (stopped_.load(std::memory_order_acquire) || !fanout_) {
        return std::nullopt;
    }
    return fanout_->Pop(channel_uuid);
}

void BotSession::OnTargetClose(std::string_view channel_uuid, int direction) noexcept {
    osw::log::Debug(kSubsystem,
                    "BotSession target close bot_id=%s channel=%.*s direction=%d",
                    cfg_.bot_id.c_str(),
                    static_cast<int>(channel_uuid.size()),
                    channel_uuid.data(),
                    direction);
}

bool BotSession::IsStopped() const noexcept {
    return stopped_.load(std::memory_order_acquire);
}

std::uint64_t BotSession::FramesSentUpstream() const noexcept {
    return frames_sent_.load(std::memory_order_relaxed);
}

std::uint64_t BotSession::FramesReceivedFromUpstream() const noexcept {
    return frames_recv_.load(std::memory_order_relaxed);
}

std::uint64_t BotSession::TargetDropCount() const noexcept {
    return fanout_ ? fanout_->TotalDropped() : 0;
}

const std::vector<std::string>& BotSession::TargetUuids() const noexcept {
    return cfg_.target_channel_uuids;
}

const std::vector<BugHandle>& BotSession::BugHandles() const noexcept {
    return handles_;
}

bool BotSession::SupportsRead() const noexcept {
    return cfg_.purpose == StartBotRequest::STT_LISTEN ||
           cfg_.purpose == StartBotRequest::VOICEBOT_DUPLEX ||
           cfg_.purpose == StartBotRequest::WHISPER;
}

bool BotSession::SupportsWrite() const noexcept {
    return cfg_.purpose == StartBotRequest::TTS_BROADCAST ||
           cfg_.purpose == StartBotRequest::VOICEBOT_DUPLEX ||
           cfg_.purpose == StartBotRequest::WHISPER;
}

bool BotSession::ShouldAttachWrite(std::string_view channel_uuid) const noexcept {
    if (!SupportsWrite()) {
        return false;
    }
    if (cfg_.purpose != StartBotRequest::WHISPER) {
        return true;
    }
    return std::find(cfg_.write_target_channel_uuids.begin(),
                     cfg_.write_target_channel_uuids.end(),
                     channel_uuid) != cfg_.write_target_channel_uuids.end();
}

std::uint32_t BotSession::QueueCapacityFrames() const noexcept {
    return std::max<std::uint32_t>(1, cfg_.target_queue_ms / kDefaultPtimeMs);
}

}  // namespace osw::media
