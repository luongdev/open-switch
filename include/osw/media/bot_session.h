/*
 * include/osw/media/bot_session.h
 *
 * W7 Track D logical bot session. One BotSession owns one upstream
 * MediaBridge stream and fans service audio out to per-target
 * WRITE_REPLACE queues.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_BOT_SESSION_H_
#define OSW_MEDIA_BOT_SESSION_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/media/audio_frame.h"
#include "osw/media/bug_handle.h"
#include "osw/media/resampler.h"

namespace osw::media {

class BugFanout;
class MediaBugManager;
class StreamClient;

struct BotSessionConfig {
    std::string bot_id;
    std::string tenant_id;
    std::string upstream_endpoint;
    std::string traceparent;
    std::vector<std::string> target_channel_uuids;
    std::vector<std::string> write_target_channel_uuids;
    open_switch::control::v1::StartBotRequest::Purpose purpose =
        open_switch::control::v1::StartBotRequest::PURPOSE_UNSPECIFIED;
    std::uint32_t sample_rate_hz = 16000;
    std::string start_message;
    std::map<std::string, std::string> variables;
    std::uint32_t target_queue_ms = 500;
    std::uint32_t drain_timeout_ms = 2000;
};

class BotSession;

/// Context passed to the W7 bot read-tap callback. Owned by BotSession and
/// valid until its BugHandle has detached.
struct BotReadTapCtx {
    BotSession* bot = nullptr;
    std::string channel_uuid;
    std::uint32_t stream_rate_hz = 0;
    std::uint32_t fs_rate_hz = 0;
    std::unique_ptr<Resampler> resampler;
    bool resampler_error_logged = false;
};

/// Context passed to the W7 bot write-replace callback. Owned by BotSession
/// and valid until its BugHandle has detached.
struct BotWriteReplaceCtx {
    BotSession* bot = nullptr;
    std::string channel_uuid;
    std::uint32_t fs_rate_hz = 0;
    std::unique_ptr<Resampler> resampler;
    bool resampler_error_logged = false;
    bool first_set_frame_logged = false;
};

/// One logical bot entity. The first landed Track D slice supports the
/// TTS_BROADCAST server-audio path: one upstream stream feeding one
/// BugFanout, with one WRITE_REPLACE bug and queue per target.
class BotSession {
  public:
    BotSession(BotSessionConfig cfg, std::shared_ptr<grpc::Channel> channel) noexcept;
    ~BotSession() noexcept;

    BotSession(const BotSession&) = delete;
    BotSession& operator=(const BotSession&) = delete;
    BotSession(BotSession&&) = delete;
    BotSession& operator=(BotSession&&) = delete;

    /// Open the single upstream stream. Idempotent.
    grpc::Status Open(int open_deadline_ms = 5000) noexcept;

    /// Attach the per-target media bugs. For TTS_BROADCAST this attaches one
    /// WRITE_REPLACE bug per target and wires `write_replace_callback`.
    grpc::Status Attach(MediaBugManager& mgr,
                        void* write_replace_callback,
                        void* read_tap_callback = nullptr) noexcept;

    /// Detach all bugs, stop accepting fanout pushes, and close the upstream
    /// stream. Idempotent.
    void Stop() noexcept;

    /// Read-side entry point for STT_LISTEN/VOICEBOT_DUPLEX/WHISPER. Frames
    /// are tagged with the source channel UUID on the shared bot stream.
    void OnTargetReadFrame(std::string_view channel_uuid,
                           std::uint64_t fs_timestamp_samples,
                           const std::int16_t* samples,
                           std::size_t sample_count,
                           std::uint32_t sample_rate_hz,
                           std::uint32_t channels = 1) noexcept;

    /// Pop the next service audio frame for a target, or nullopt when the bot
    /// is silent. Callers must passthrough unchanged on nullopt.
    [[nodiscard]] std::optional<AudioFrame> PopWriteFrame(std::string_view channel_uuid) noexcept;

    void OnTargetClose(std::string_view channel_uuid, int direction) noexcept;

    [[nodiscard]] bool IsStopped() const noexcept;
    [[nodiscard]] std::uint64_t FramesSentUpstream() const noexcept;
    [[nodiscard]] std::uint64_t FramesReceivedFromUpstream() const noexcept;
    [[nodiscard]] std::uint64_t TargetDropCount() const noexcept;
    [[nodiscard]] const std::vector<std::string>& TargetUuids() const noexcept;
    [[nodiscard]] const std::vector<BugHandle>& BugHandles() const noexcept;

  private:
    [[nodiscard]] bool SupportsRead() const noexcept;
    [[nodiscard]] bool SupportsWrite() const noexcept;
    [[nodiscard]] bool ShouldAttachWrite(std::string_view channel_uuid) const noexcept;
    [[nodiscard]] std::uint32_t QueueCapacityFrames() const noexcept;

    BotSessionConfig cfg_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<StreamClient> client_;
    std::unique_ptr<BugFanout> fanout_;
    std::vector<BugHandle> handles_;
    std::vector<std::unique_ptr<BotReadTapCtx>> read_contexts_;
    std::vector<std::unique_ptr<BotWriteReplaceCtx>> write_contexts_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> opened_{false};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
};

}  // namespace osw::media

#endif  // OSW_MEDIA_BOT_SESSION_H_
