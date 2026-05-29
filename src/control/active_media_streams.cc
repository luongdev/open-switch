/*
 * src/control/active_media_streams.cc
 *
 * ActiveMediaStreams implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/active_media_streams.h"

#include "osw/media/recording_relay.h"

#include "src/control/handlers/media_bug_callbacks.h"

namespace osw::control {

// WriteCtxDeleter: typed deleter for the WriteCallbackCtx void* owner.
void WriteCtxDeleter::operator()(void* p) const noexcept {
    delete static_cast<osw::control::handlers::WriteCallbackCtx*>(p);
}

void RecordingCtxDeleter::operator()(void* p) const noexcept {
    delete static_cast<osw::media::RecordingRelay*>(p);
}

}  // namespace osw::control

#include <utility>
#include <vector>

#include "osw/observability/log.h"

namespace osw::control {

namespace {
constexpr const char* kSubsystem = "control.active_media_streams";

bool IsWriteReplacePurpose(open_switch::media::v1::StreamStart::Purpose purpose) noexcept {
    using StreamStart = open_switch::media::v1::StreamStart;
    return purpose == StreamStart::TTS_PLAYBACK || purpose == StreamStart::VOICEBOT_DUPLEX;
}
}  // namespace

ActiveMediaStreams::~ActiveMediaStreams() noexcept {
    // On module shutdown, forcibly tear down any surviving streams.
    std::unique_lock<std::mutex> lk(mu_);
    // Drain under lock so we see a consistent snapshot, then tear down
    // each stream outside the lock (Close() can block on joining threads).
    std::vector<std::unique_ptr<ActiveMediaStream>> all;
    all.reserve(by_id_.size());
    for (auto& [_, s] : by_id_) {
        all.push_back(std::move(s));
    }
    by_id_.clear();
    lk.unlock();

    for (auto& s : all) {
        osw::log::Debug(kSubsystem,
                        "~ActiveMediaStreams: tearing down stream_id=%s channel=%s",
                        s->stream_id.c_str(),
                        s->channel_uuid.c_str());
        TearDown(std::move(s));
    }
}

bool ActiveMediaStreams::Insert(std::unique_ptr<ActiveMediaStream> s) noexcept {
    if (!s || s->stream_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> g(mu_);
    const auto [it, ok] = by_id_.emplace(s->stream_id, std::move(s));
    return ok;
}

bool ActiveMediaStreams::Remove(std::string_view stream_id) noexcept {
    std::unique_ptr<ActiveMediaStream> owned;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_id_.find(std::string(stream_id));
        if (it == by_id_.end()) {
            return false;
        }
        owned = std::move(it->second);
        by_id_.erase(it);
    }
    // Tear down outside the lock so client->Close() (which joins threads)
    // doesn't hold mu_ during potentially long blocking.
    TearDown(std::move(owned));
    return true;
}

bool ActiveMediaStreams::RemoveIfPurpose(
    std::string_view stream_id, open_switch::media::v1::StreamStart::Purpose purpose) noexcept {
    std::unique_ptr<ActiveMediaStream> owned;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_id_.find(std::string(stream_id));
        if (it == by_id_.end() || !it->second || it->second->purpose != purpose) {
            return false;
        }
        owned = std::move(it->second);
        by_id_.erase(it);
    }
    TearDown(std::move(owned));
    return true;
}

void ActiveMediaStreams::RemoveForChannel(std::string_view channel_uuid) noexcept {
    std::vector<std::unique_ptr<ActiveMediaStream>> victims;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto it = by_id_.begin(); it != by_id_.end();) {
            if (it->second && it->second->channel_uuid == channel_uuid) {
                victims.push_back(std::move(it->second));
                it = by_id_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : victims) {
        TearDown(std::move(s));
    }
}

std::size_t ActiveMediaStreams::RemovePurposeForChannel(
    std::string_view channel_uuid, open_switch::media::v1::StreamStart::Purpose purpose) noexcept {
    std::vector<std::unique_ptr<ActiveMediaStream>> victims;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto it = by_id_.begin(); it != by_id_.end();) {
            if (it->second && it->second->channel_uuid == channel_uuid &&
                it->second->purpose == purpose) {
                victims.push_back(std::move(it->second));
                it = by_id_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : victims) {
        TearDown(std::move(s));
    }
    return victims.size();
}

std::size_t ActiveMediaStreams::RemoveWriteReplaceForChannel(
    std::string_view channel_uuid) noexcept {
    std::vector<std::unique_ptr<ActiveMediaStream>> victims;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto it = by_id_.begin(); it != by_id_.end();) {
            if (it->second && it->second->channel_uuid == channel_uuid &&
                IsWriteReplacePurpose(it->second->purpose)) {
                victims.push_back(std::move(it->second));
                it = by_id_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : victims) {
        osw::log::Info(kSubsystem,
                       "RemoveWriteReplaceForChannel: tearing down prior stream_id=%s channel=%s",
                       s->stream_id.c_str(),
                       s->channel_uuid.c_str());
        TearDown(std::move(s));
    }
    return victims.size();
}

std::size_t ActiveMediaStreams::Size() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return by_id_.size();
}

// static
void ActiveMediaStreams::TearDown(std::unique_ptr<ActiveMediaStream> s) noexcept {
    if (!s) {
        return;
    }
    if (s->recording_ctx) {
        auto* relay = static_cast<osw::media::RecordingRelay*>(s->recording_ctx.get());
        relay->Stop();
        // Fence FS callbacks before closing the StreamClient that they use.
        s->bugs.clear();
        relay->EmitStopped();
        s->recording_ctx.reset();
        if (s->client) {
            s->client->Close();
            s->client.reset();
        }
        return;
    }
    // Step 1: Close the StreamClient (half-close upstream, join reader thread).
    //   This stops the gRPC reader pushing more frames into the jitter buffer,
    //   but does NOT fence the FS media thread — that thread will keep firing
    //   WRITE_REPLACE callbacks until the bug is detached in Step 3.
    if (s->client) {
        s->client->Close();
    }
    // Step 2: Signal EOS on the jitter buffer (drains remaining frames). The
    //   FS media thread may still pop frames between here and Step 3 — that's
    //   fine, the buffer's mu_ guards it.
    if (s->tts_buffer) {
        s->tts_buffer->SignalEndOfStream();
    }
    // Step 3 (W6.5 P1-002 fix — UAF closed):
    //   Clear bugs FIRST.  Each BugHandle dtor calls MediaBugManager::Detach
    //   → switch_core_media_bug_remove(session, &bug) which FS guarantees
    //   fences out further callback invocations on that bug.  After
    //   bugs.clear() returns, the FS media thread will no longer dereference
    //   write_ctx.
    //
    //   The previous order (write_ctx.reset() before bugs.clear()) freed the
    //   callback context while the FS media thread could still call
    //   OswStreamingWriteReplace, which casts user_data to WriteCallbackCtx*
    //   and reads from it → UAF caught by codex W6 review.
    s->bugs.clear();
    // Step 4: Now safe to free write_ctx — FS callbacks are fenced.
    s->write_ctx.reset();
    s->recording_ctx.reset();
    // Step 5: Release buffer.
    s->tts_buffer.reset();
    // Step 6: Release StreamClient.
    s->client.reset();
}

}  // namespace osw::control
