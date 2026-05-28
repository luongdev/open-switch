/*
 * src/media/silence_driver.cc
 *
 * W6.6 silence driver implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/raii/fs_api.h"  // FS/mock types must be available first.

#include "osw/media/silence_driver.h"

#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/session_lock.h"

namespace osw::media {

namespace {

constexpr const char* kSubsystem = "media";
constexpr const char* kSilenceStream = "silence_stream://-1";
constexpr switch_media_flag_t kSilenceBroadcastFlags =
    static_cast<switch_media_flag_t>(SMF_ECHO_ALEG | SMF_PRIORITY);
constexpr const char* kAuditStarted = "osw.media.silence_driver.started";
constexpr const char* kAuditStopped = "osw.media.silence_driver.stopped";
constexpr const char* kAuditCapReached = "osw.media.silence_driver.cap_reached";

bool ChannelAlreadyHasWriteSource(switch_channel_t* channel) noexcept {
    const char* app = osw::raii::fs::ChannelGetVariable(channel, "current_application");
    return app && std::strcmp(app, "playback") == 0;
}

}  // namespace

SilenceDriver::SilenceDriver(std::string channel_uuid, SilenceDriverRegistry& registry)
    : channel_uuid_(std::move(channel_uuid)) {
    (void)registry;
    const switch_status_t status = osw::raii::fs::IvrBroadcast(
        channel_uuid_.c_str(), kSilenceStream, kSilenceBroadcastFlags);
    if (status == SWITCH_STATUS_SUCCESS) {
        running_.store(true, std::memory_order_release);
    } else {
        osw::log::Warn(kSubsystem,
                       "failed to queue silence broadcast for channel '%s': status=%d",
                       channel_uuid_.c_str(),
                       static_cast<int>(status));
    }
}

SilenceDriver::~SilenceDriver() { Stop(); }

void SilenceDriver::Stop() noexcept {
    const bool already_requested = stop_requested_.exchange(true, std::memory_order_acq_rel);
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!already_requested) {
        osw::SessionLock session(channel_uuid_.c_str());
        if (session) {
            switch_channel_t* channel = session.channel();
            if (channel) {
                osw::raii::fs::ChannelSetFlag(channel, CF_BREAK);
            }
        }
    }

    if (was_running) {
        osw::audit::Emit(kAuditStopped, {{"Unique-ID", channel_uuid_}});
    }
}

bool SilenceDriver::IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

SilenceDriverRegistry::SilenceDriverRegistry(const Config& config) noexcept : config_(config) {}

SilenceDriverRegistry::~SilenceDriverRegistry() {
    DrainAll();
}

void SilenceDriverRegistry::AttachOpportunistic(switch_core_session_t* session) noexcept {
    if (!config_.silence_driver_enabled || !session) {
        return;
    }

    const char* uuid_cstr = osw::raii::fs::SessionGetUuid(session);
    if (!uuid_cstr || uuid_cstr[0] == '\0') {
        return;
    }
    std::string uuid(uuid_cstr);

    switch_channel_t* channel = osw::raii::fs::SessionGetChannel(session);
    if (!channel) {
        return;
    }
    if (osw::raii::fs::ChannelTestFlag(channel, CF_BROADCAST) != 0) {
        return;
    }
    if (osw::raii::fs::ChannelTestFlag(channel, CF_BRIDGED) != 0) {
        return;
    }
    if (ChannelAlreadyHasWriteSource(channel)) {
        osw::log::Debug(kSubsystem,
                        "skip silence driver for channel '%s': current_application=playback",
                        uuid.c_str());
        return;
    }

    try {
        std::lock_guard<std::mutex> lk(mu_);
        if (drivers_.find(uuid) != drivers_.end()) {
            return;
        }
        if (drivers_.size() >= config_.max_silence_drivers) {
            osw::audit::Emit(kAuditCapReached,
                             {{"Unique-ID", uuid},
                              {"cap", std::to_string(config_.max_silence_drivers)}});
            return;
        }

        auto driver = std::make_unique<SilenceDriver>(uuid, *this);
        if (!driver->IsRunning()) {
            return;
        }
        drivers_.emplace(uuid, std::move(driver));
        osw::audit::Emit(kAuditStarted, {{"Unique-ID", uuid}});
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem,
                        "failed to start silence driver for channel '%s': %s",
                        uuid.c_str(),
                        e.what());
    } catch (...) {
        osw::log::Error(kSubsystem,
                        "failed to start silence driver for channel '%s': unknown exception",
                        uuid.c_str());
    }
}

void SilenceDriverRegistry::DetachIfOrphan(std::string_view channel_uuid) noexcept {
    StopAndErase(channel_uuid);
}

void SilenceDriverRegistry::RemoveForChannel(std::string_view channel_uuid) noexcept {
    StopAndErase(channel_uuid);
}

void SilenceDriverRegistry::DrainAll() noexcept {
    std::vector<std::unique_ptr<SilenceDriver>> drivers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        drivers.reserve(drivers_.size());
        for (auto& entry : drivers_) {
            drivers.push_back(std::move(entry.second));
        }
        drivers_.clear();
    }

    for (auto& driver : drivers) {
        if (driver) {
            driver->Stop();
        }
    }
}

std::size_t SilenceDriverRegistry::ActiveCount() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return drivers_.size();
}

void SilenceDriverRegistry::StopAndErase(std::string_view channel_uuid) noexcept {
    std::unique_ptr<SilenceDriver> driver;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = drivers_.find(std::string(channel_uuid));
        if (it == drivers_.end()) {
            return;
        }
        driver = std::move(it->second);
        drivers_.erase(it);
    }

    if (driver) {
        driver->Stop();
    }
}

}  // namespace osw::media
