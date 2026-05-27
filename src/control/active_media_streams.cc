/*
 * src/control/active_media_streams.cc
 *
 * ActiveMediaStreams implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/active_media_streams.h"

#include "src/control/handlers/media_bug_callbacks.h"

namespace osw::control {

// WriteCtxDeleter: typed deleter for the WriteCallbackCtx void* owner.
void WriteCtxDeleter::operator()(void* p) const noexcept {
    delete static_cast<osw::control::handlers::WriteCallbackCtx*>(p);
}

}  // namespace osw::control

#include <utility>
#include <vector>

#include "osw/observability/log.h"

namespace osw::control {

namespace {
constexpr const char* kSubsystem = "control.active_media_streams";
}

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

std::size_t ActiveMediaStreams::Size() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return by_id_.size();
}

// static
void ActiveMediaStreams::TearDown(std::unique_ptr<ActiveMediaStream> s) noexcept {
    if (!s) {
        return;
    }
    // Step 1: Close the StreamClient (half-close upstream, join reader thread).
    if (s->client) {
        s->client->Close();
    }
    // Step 2: Signal EOS on the jitter buffer (drains remaining frames).
    if (s->tts_buffer) {
        s->tts_buffer->SignalEndOfStream();
    }
    // Step 3: Clear write_ctx BEFORE bugs so the bug callback can no longer
    // dereference it (reader thread joined in step 1 means no concurrent use).
    s->write_ctx.reset();
    // Step 4: Clear bugs — BugHandle dtors call MediaBugManager::Detach.
    // The FS bug callback is fully fenced (reader thread joined above) before
    // any BugHandle dtor runs.
    s->bugs.clear();
    // Step 5: Release buffer.
    s->tts_buffer.reset();
    // Step 6: Release StreamClient.
    s->client.reset();
}

}  // namespace osw::control
