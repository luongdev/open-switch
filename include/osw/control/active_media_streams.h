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
    /// WriteCallbackCtx heap allocation. Freed before bugs.clear() in TearDown.
    std::unique_ptr<void, WriteCtxDeleter> write_ctx;
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
    bool Insert(std::unique_ptr<ActiveMediaStream> s) noexcept;

    /// Remove and tear down. Idempotent — returns false if not present.
    /// Teardown ordering per spec: client->Close() then
    /// buffer->SignalEndOfStream() then bugs.clear() then buffer.reset().
    bool Remove(std::string_view stream_id) noexcept;

    /// Remove every stream for a channel (called from CS_DESTROY hook
    /// alongside MediaBugManager::DetachAll). Idempotent.
    void RemoveForChannel(std::string_view channel_uuid) noexcept;

    /// Remove write-replace playback streams for a channel before attaching a
    /// new speaker-side media bug. Leaves read-only STT streams untouched.
    std::size_t RemoveWriteReplaceForChannel(std::string_view channel_uuid) noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<ActiveMediaStream>> by_id_;

    /// Execute teardown for one stream (assumes caller has removed it from
    /// by_id_ already; called without mu_ held so client->Close() can block).
    static void TearDown(std::unique_ptr<ActiveMediaStream> s) noexcept;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_ACTIVE_MEDIA_STREAMS_H_
