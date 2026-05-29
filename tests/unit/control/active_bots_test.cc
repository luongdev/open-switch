/*
 * tests/unit/control/active_bots_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/active_bots.h"

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include "osw/media/bot_session.h"

namespace {

using open_switch::control::v1::StartBotRequest;

std::unique_ptr<osw::media::BotSession> MakeBotSession(std::string bot_id,
                                                       std::string channel_uuid) {
    osw::media::BotSessionConfig cfg;
    cfg.bot_id = std::move(bot_id);
    cfg.tenant_id = "tenant";
    cfg.target_channel_uuids = {std::move(channel_uuid)};
    cfg.purpose = StartBotRequest::TTS_BROADCAST;
    cfg.sample_rate_hz = 8000;
    auto channel = grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
    return std::make_unique<osw::media::BotSession>(std::move(cfg), std::move(channel));
}

TEST(ActiveBotsTest, InsertIndexesBotSessionTargets) {
    osw::control::ActiveBots bots;

    osw::control::ActiveBot bot;
    bot.bot_id = "bot-1";
    bot.target_channel_uuids = {"chan-a"};
    bot.session = MakeBotSession("bot-1", "chan-a");
    auto* raw_session = bot.session.get();

    EXPECT_EQ(bots.Insert(std::move(bot), /*max_bots_per_channel=*/1),
              osw::control::ActiveBotInsertResult::kInserted);
    EXPECT_TRUE(bots.Contains("bot-1"));
    EXPECT_EQ(bots.Find("bot-1"), raw_session);
    EXPECT_TRUE(bots.ChannelAtCapacity("chan-a", /*max_bots_per_channel=*/1));
    EXPECT_EQ(bots.ActiveCount(), 1u);

    bots.DrainAll(nullptr);
    EXPECT_EQ(bots.ActiveCount(), 0u);
}

TEST(ActiveBotsTest, ChannelCapacityRejectsSecondBotOnSameChannel) {
    osw::control::ActiveBots bots;

    osw::control::ActiveBot first;
    first.bot_id = "bot-1";
    first.target_channel_uuids = {"chan-a"};
    first.session = MakeBotSession("bot-1", "chan-a");
    ASSERT_EQ(bots.Insert(std::move(first), 1), osw::control::ActiveBotInsertResult::kInserted);

    osw::control::ActiveBot second;
    second.bot_id = "bot-2";
    second.target_channel_uuids = {"chan-a"};
    second.session = MakeBotSession("bot-2", "chan-a");

    EXPECT_EQ(bots.Insert(std::move(second), 1),
              osw::control::ActiveBotInsertResult::kChannelCapacityExceeded);
    EXPECT_TRUE(bots.Contains("bot-1"));
    EXPECT_FALSE(bots.Contains("bot-2"));
}

TEST(ActiveBotsTest, StopByChannelStopsOnlyMatchingBots) {
    osw::control::ActiveBots bots;

    osw::control::ActiveBot first;
    first.bot_id = "bot-1";
    first.target_channel_uuids = {"chan-a"};
    first.session = MakeBotSession("bot-1", "chan-a");
    ASSERT_TRUE(bots.Insert(std::move(first)));

    osw::control::ActiveBot second;
    second.bot_id = "bot-2";
    second.target_channel_uuids = {"chan-b"};
    second.session = MakeBotSession("bot-2", "chan-b");
    ASSERT_TRUE(bots.Insert(std::move(second)));

    EXPECT_EQ(bots.StopByChannel("chan-a", nullptr), 1u);
    EXPECT_FALSE(bots.Contains("bot-1"));
    EXPECT_TRUE(bots.Contains("bot-2"));
    EXPECT_EQ(bots.ActiveCount(), 1u);
}

}  // namespace
