/*
 * include/osw/control/active_media_streams.h
 *
 * osw::control::ActiveMediaStreams — per-Module registry keyed by
 * stream_id (UUIDv7) that owns the lifecycle of each active media stream:
 * BugHandle(s) + StreamClient + optional TtsPlayoutBuffer.
 *
 * Per-stream teardown ordering for TTS/voicebot-write streams:
 *   1. client->Close()              -- half-close upstream; joins reader
 *   2. buffer->SignalEndOfStream()  -- drain remaining frames
 *   3. bugs.clear()                 -- detach via BugHandle dtors
 *   4. buffer.reset()
 *
 * The ordering ensures the reader thread is fully joined before the
 * bug callback can race with buffer destruction.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_ACTIVE_MEDIA_STREAMS_H_
#define OSW_CONTROL_ACTIVE_MEDIA_STREAMS_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/bug_handle.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"

namespace osw::control {

/// Opaque write-callback context (defined in media_bug_callbacks.h).
/// Stored here as void* so active_media_streams.h doesn't include the
/// handler-private header. The typed deleter is set at construction.
struct WriteCtxDeleter {
    void operator()(void* p) const noexcept;
};

struct ReadCtxDeleter {
    void operator()(void* p) const noexcept;
};

struct RecordingCtxDeleter {
    void operator()(void* p) const noexcept;
};

/// Per-stream data owned by the registry.
struct ActiveMediaStream {
    std::string channel_uuid;
    std::string stream_id;
    open_switch::media::v1::StreamStart::Purpose purpose;
    /// One handle for STT / TTS; two for voicebot (read + write).
    std::vector<osw::media::BugHandle> bugs;
    std::unique_ptr<osw::media::StreamClient> client;
    /// Non-null for TTS / voicebot-write streams; null for STT.
    std::unique_ptr<osw::media::TtsPlayoutBuffer> tts_buffer;
    /// WriteCallbackCtx heap allocation. Freed after bugs.clear() in TearDown.
    std::unique_ptr<void, WriteCtxDeleter> write_ctx;
    /// ReadCallbackCtx heap allocation. Freed after bugs.clear() in TearDown.
    std::unique_ptr<void, ReadCtxDeleter> read_ctx;
    /// RecordingRelay heap allocation. Stopped before client close and freed
    /// after media bugs are detached.
    std::unique_ptr<void, RecordingCtxDeleter> recording_ctx;
};

/// Non-singleton registry owned by Module, injected into handlers via
/// ControlServiceSkeleton::SetActiveMediaStreams.
class ActiveMediaStreams {
  public:
    ActiveMediaStreams() noexcept = default;
    ~ActiveMediaStreams() noexcept;

    ActiveMediaStreams(const ActiveMediaStreams&) = delete;
    ActiveMediaStreams& operator=(const ActiveMediaStreams&) = delete;
    ActiveMediaStreams(ActiveMediaStreams&&) = delete;
    ActiveMediaStreams& operator=(ActiveMediaStreams&&) = delete;

    /// Insert and take ownership. Returns false if stream_id already exists.
    /// Rejected non-null streams are torn down with the same callback-fencing
    /// order used by Remove().
    bool Insert(std::unique_ptr<ActiveMediaStream> s) noexcept;

    /// Remove and tear down. Idempotent — returns false if not present.
    /// Teardown ordering per spec: client->Close() then
    /// buffer->SignalEndOfStream() then bugs.clear() then buffer.reset().
    bool Remove(std::string_view stream_id) noexcept;

    /// Remove only when the stream exists and has the requested media-plane
    /// purpose. Returns false for unknown ids or purpose mismatch.
    bool RemoveIfPurpose(std::string_view stream_id,
                         open_switch::media::v1::StreamStart::Purpose purpose) noexcept;

    /// Remove every stream for a channel (called from CS_DESTROY hook
    /// alongside MediaBugManager::DetachAll). Idempotent.
    void RemoveForChannel(std::string_view channel_uuid) noexcept;

    /// Remove write-replace playback streams for a channel before attaching a
    /// new speaker-side media bug. Leaves read-only STT streams untouched.
    std::size_t RemoveWriteReplaceForChannel(std::string_view channel_uuid) noexcept;

    /// Remove all streams on a channel that match a purpose.
    std::size_t RemovePurposeForChannel(
        std::string_view channel_uuid,
        open_switch::media::v1::StreamStart::Purpose purpose) noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;

    /// Wire the shared TTS-playout Prometheus metrics (non-owning; owned by the
    /// Module's registry). Handlers read these via the getters below and attach
    /// them to each TtsPlayoutBuffer they create. Null when metrics disabled.
    void SetTtsMetrics(observability::prometheus::Histogram* first_audio_latency,
                       observability::prometheus::Counter* underrun_total) noexcept {
        tts_first_audio_latency_ = first_audio_latency;
        tts_underrun_total_ = underrun_total;
    }
    [[nodiscard]] observability::prometheus::Histogram* TtsFirstAudioLatency() const noexcept {
        return tts_first_audio_latency_;
    }
    [[nodiscard]] observability::prometheus::Counter* TtsUnderrunTotal() const noexcept {
        return tts_underrun_total_;
    }

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<ActiveMediaStream>> by_id_;

    // Shared TTS-playout metrics (non-owning; set once at module init).
    observability::prometheus::Histogram* tts_first_audio_latency_ = nullptr;
    observability::prometheus::Counter* tts_underrun_total_ = nullptr;

    /// Execute teardown for one stream (assumes caller has removed it from
    /// by_id_ already; called without mu_ held so client->Close() can block).
    static void TearDown(std::unique_ptr<ActiveMediaStream> s) noexcept;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_ACTIVE_MEDIA_STREAMS_H_
