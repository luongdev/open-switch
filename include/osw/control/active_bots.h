/*
 * include/osw/control/active_bots.h
 *
 * W7 Track D logical bot registry. The registry owns bot metadata and
 * delegates concrete media teardown to ActiveMediaStreams.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_ACTIVE_BOTS_H_
#define OSW_CONTROL_ACTIVE_BOTS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osw::media {
class BotSession;
}  // namespace osw::media

namespace osw::control {

class ActiveMediaStreams;

struct ActiveBot {
    ActiveBot() noexcept;
    ~ActiveBot() noexcept;
    ActiveBot(ActiveBot&&) noexcept;
    ActiveBot& operator=(ActiveBot&&) noexcept;
    ActiveBot(const ActiveBot&) = delete;
    ActiveBot& operator=(const ActiveBot&) = delete;

    std::string bot_id;
    std::vector<std::string> target_channel_uuids;
    /// Legacy facade stream ids. Empty for real Track D BotSession-backed bots.
    std::vector<std::string> stream_ids;
    /// Real Track D owner. When non-null, Stop/Drain calls BotSession::Stop().
    std::unique_ptr<osw::media::BotSession> session;
};

enum class ActiveBotInsertResult {
    kInserted,
    kDuplicateBotId,
    kChannelCapacityExceeded,
};

class ActiveBots {
  public:
    ActiveBots() noexcept = default;
    ~ActiveBots() noexcept = default;

    ActiveBots(const ActiveBots&) = delete;
    ActiveBots& operator=(const ActiveBots&) = delete;
    ActiveBots(ActiveBots&&) = delete;
    ActiveBots& operator=(ActiveBots&&) = delete;

    bool Insert(ActiveBot bot) noexcept;
    ActiveBotInsertResult Insert(ActiveBot bot, std::uint32_t max_bots_per_channel) noexcept;
    void Register(std::string bot_id, std::unique_ptr<osw::media::BotSession> bot) noexcept;
    [[nodiscard]] bool Contains(std::string_view bot_id) const noexcept;
    [[nodiscard]] osw::media::BotSession* Find(std::string_view bot_id) const noexcept;
    [[nodiscard]] bool ChannelAtCapacity(std::string_view channel_uuid,
                                         std::uint32_t max_bots_per_channel) const noexcept;

    /// Stop and remove one bot. Idempotent: false means bot_id was unknown.
    bool Stop(std::string_view bot_id, ActiveMediaStreams* streams) noexcept;

    /// Stop every bot attached to a channel and erase its metadata.
    std::size_t StopByChannel(std::string_view channel_uuid, ActiveMediaStreams* streams) noexcept;

    /// Stop all bots during module shutdown.
    void DrainAll(ActiveMediaStreams* streams) noexcept;

    [[nodiscard]] std::size_t ActiveCount() const noexcept;

  private:
    static void TearDown(ActiveBot bot, ActiveMediaStreams* streams) noexcept;
    static void EraseValue(std::vector<std::string>& values, std::string_view value) noexcept;

    mutable std::mutex mu_;
    std::unordered_map<std::string, ActiveBot> by_id_;
    std::unordered_map<std::string, std::vector<std::string>> by_channel_;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_ACTIVE_BOTS_H_
