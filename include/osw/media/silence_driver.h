/*
 * include/osw/media/silence_driver.h
 *
 * W6.6 silence driver — module-owned write-side frame source for
 * freshly answered / parked channels that have WRITE_REPLACE media bugs
 * but no active playback, bridge, or broadcast source.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_SILENCE_DRIVER_H_
#define OSW_MEDIA_SILENCE_DRIVER_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "osw/core/config.h"

#if !defined(OSW_TEST_FS_MOCK)
struct switch_core_session;
using switch_core_session_t = switch_core_session;
#endif  // !OSW_TEST_FS_MOCK

namespace osw::media {

class SilenceDriverRegistry;

/// One per-channel silence playback thread. Spawned by the registry
/// when a WRITE_REPLACE bug attaches and the channel has no other
/// write-side source.
class SilenceDriver {
  public:
    explicit SilenceDriver(std::string channel_uuid, SilenceDriverRegistry& registry);
    ~SilenceDriver();

    SilenceDriver(const SilenceDriver&) = delete;
    SilenceDriver& operator=(const SilenceDriver&) = delete;
    SilenceDriver(SilenceDriver&&) = delete;
    SilenceDriver& operator=(SilenceDriver&&) = delete;

    /// Set CF_BREAK on the channel and join the driver thread.
    /// Idempotent.
    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

  private:
    void Run() noexcept;

    std::string channel_uuid_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> stop_requested_{false};
};

class SilenceDriverRegistry {
  public:
    explicit SilenceDriverRegistry(const Config& config) noexcept;
    ~SilenceDriverRegistry();

    SilenceDriverRegistry(const SilenceDriverRegistry&) = delete;
    SilenceDriverRegistry& operator=(const SilenceDriverRegistry&) = delete;
    SilenceDriverRegistry(SilenceDriverRegistry&&) = delete;
    SilenceDriverRegistry& operator=(SilenceDriverRegistry&&) = delete;

    /// Opportunistically starts a per-channel silence driver when:
    /// config enables it, no driver is already present, the channel is
    /// not broadcasting, the channel is not bridged, and the active-driver
    /// cap has not been reached.
    void AttachOpportunistic(switch_core_session_t* session) noexcept;

    /// Stops the module-owned driver after the last WRITE_REPLACE bug on
    /// the channel detaches.
    void DetachIfOrphan(std::string_view channel_uuid) noexcept;

    /// Stops any driver for a channel during CS_DESTROY cleanup.
    void RemoveForChannel(std::string_view channel_uuid) noexcept;

    /// Stops and joins every active driver. Called from module shutdown.
    void DrainAll() noexcept;

    [[nodiscard]] std::size_t ActiveCount() const noexcept;

  private:
    void StopAndErase(std::string_view channel_uuid) noexcept;

    const Config& config_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<SilenceDriver>> drivers_;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_SILENCE_DRIVER_H_
