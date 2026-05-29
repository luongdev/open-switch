/*
 * src/control/active_bots.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/active_bots.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "osw/control/active_media_streams.h"
#include "osw/media/bot_session.h"

namespace osw::control {

ActiveBot::ActiveBot() noexcept = default;
ActiveBot::~ActiveBot() noexcept = default;
ActiveBot::ActiveBot(ActiveBot&&) noexcept = default;
ActiveBot& ActiveBot::operator=(ActiveBot&&) noexcept = default;

bool ActiveBots::Insert(ActiveBot bot) noexcept {
    return Insert(std::move(bot), std::numeric_limits<std::uint32_t>::max()) ==
           ActiveBotInsertResult::kInserted;
}

ActiveBotInsertResult ActiveBots::Insert(ActiveBot bot,
                                         std::uint32_t max_bots_per_channel) noexcept {
    if (bot.bot_id.empty()) {
        return ActiveBotInsertResult::kDuplicateBotId;
    }

    std::lock_guard<std::mutex> lk(mu_);
    const std::string bot_id = bot.bot_id;
    if (by_id_.find(bot_id) != by_id_.end()) {
        return ActiveBotInsertResult::kDuplicateBotId;
    }

    for (const auto& channel_uuid : bot.target_channel_uuids) {
        auto it = by_channel_.find(channel_uuid);
        const std::size_t active_count = (it == by_channel_.end()) ? 0u : it->second.size();
        if (active_count >= max_bots_per_channel) {
            return ActiveBotInsertResult::kChannelCapacityExceeded;
        }
    }

    for (const auto& channel_uuid : bot.target_channel_uuids) {
        by_channel_[channel_uuid].push_back(bot_id);
    }
    by_id_.emplace(bot_id, std::move(bot));
    return ActiveBotInsertResult::kInserted;
}

void ActiveBots::Register(std::string bot_id,
                          std::unique_ptr<osw::media::BotSession> bot) noexcept {
    if (!bot) {
        return;
    }

    ActiveBot active;
    active.bot_id = std::move(bot_id);
    active.target_channel_uuids = bot->TargetUuids();
    active.session = std::move(bot);
    (void)Insert(std::move(active));
}

bool ActiveBots::Contains(std::string_view bot_id) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return by_id_.find(std::string(bot_id)) != by_id_.end();
}

osw::media::BotSession* ActiveBots::Find(std::string_view bot_id) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(std::string(bot_id));
    if (it == by_id_.end()) {
        return nullptr;
    }
    return it->second.session.get();
}

bool ActiveBots::ChannelAtCapacity(std::string_view channel_uuid,
                                   std::uint32_t max_bots_per_channel) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_channel_.find(std::string(channel_uuid));
    return it != by_channel_.end() && it->second.size() >= max_bots_per_channel;
}

bool ActiveBots::Stop(std::string_view bot_id, ActiveMediaStreams* streams) noexcept {
    ActiveBot bot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string(bot_id));
        if (it == by_id_.end()) {
            return false;
        }
        bot = std::move(it->second);
        by_id_.erase(it);
        for (const auto& channel_uuid : bot.target_channel_uuids) {
            auto ch_it = by_channel_.find(channel_uuid);
            if (ch_it == by_channel_.end()) {
                continue;
            }
            EraseValue(ch_it->second, bot.bot_id);
            if (ch_it->second.empty()) {
                by_channel_.erase(ch_it);
            }
        }
    }

    TearDown(std::move(bot), streams);
    return true;
}

std::size_t ActiveBots::StopByChannel(std::string_view channel_uuid,
                                      ActiveMediaStreams* streams) noexcept {
    std::vector<std::string> bot_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_channel_.find(std::string(channel_uuid));
        if (it == by_channel_.end()) {
            return 0;
        }
        bot_ids = it->second;
    }

    std::size_t stopped = 0;
    for (const auto& bot_id : bot_ids) {
        if (Stop(bot_id, streams)) {
            ++stopped;
        }
    }
    return stopped;
}

void ActiveBots::DrainAll(ActiveMediaStreams* streams) noexcept {
    std::vector<ActiveBot> bots;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bots.reserve(by_id_.size());
        for (auto& entry : by_id_) {
            bots.push_back(std::move(entry.second));
        }
        by_id_.clear();
        by_channel_.clear();
    }

    for (auto& bot : bots) {
        TearDown(std::move(bot), streams);
    }
}

std::size_t ActiveBots::ActiveCount() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return by_id_.size();
}

void ActiveBots::TearDown(ActiveBot bot, ActiveMediaStreams* streams) noexcept {
    if (bot.session) {
        bot.session->Stop();
    }
    if (streams) {
        for (const auto& stream_id : bot.stream_ids) {
            streams->Remove(stream_id);
        }
    }
}

void ActiveBots::EraseValue(std::vector<std::string>& values, std::string_view value) noexcept {
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

}  // namespace osw::control
