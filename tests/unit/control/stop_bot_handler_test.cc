/*
 * tests/unit/control/stop_bot_handler_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/bug_manager.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

class StopBotHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
        bug_mgr_ = std::make_unique<osw::media::MediaBugManager>();
        streams_ = std::make_unique<osw::control::ActiveMediaStreams>();
        bots_ = std::make_unique<osw::control::ActiveBots>();
        svc_->SetMediaBugManager(bug_mgr_.get());
        svc_->SetActiveMediaStreams(streams_.get());
        svc_->SetActiveBots(bots_.get());
        svc_->SetConfig(&config_);
    }

    void InsertBot(const std::string& bot_id, const std::string& stream_id) {
        auto s = std::make_unique<osw::control::ActiveMediaStream>();
        s->channel_uuid = "chan-a";
        s->stream_id = stream_id;
        s->purpose = open_switch::media::v1::StreamStart::TTS_PLAYBACK;
        ASSERT_TRUE(streams_->Insert(std::move(s)));

        osw::control::ActiveBot bot;
        bot.bot_id = bot_id;
        bot.target_channel_uuids = {"chan-a"};
        bot.stream_ids = {stream_id};
        ASSERT_TRUE(bots_->Insert(std::move(bot)));
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    std::unique_ptr<osw::control::ActiveBots> bots_;
    osw::Config config_;
};

TEST_F(StopBotHandlerTest, NullMediaPlaneReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    svc2->SetConfig(&config_);

    open_switch::control::v1::StopBotRequest req;
    req.set_bot_id("bot-1");
    open_switch::control::v1::StopBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StopBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(StopBotHandlerTest, EmptyBotIdReturnsInvalidArgument) {
    open_switch::control::v1::StopBotRequest req;
    open_switch::control::v1::StopBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StopBotHandlerTest, UnknownBotIdReturnsOkWasActiveFalse) {
    open_switch::control::v1::StopBotRequest req;
    req.set_bot_id("missing");
    open_switch::control::v1::StopBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopBot(&ctx, &req, &resp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.was_active());
}

TEST_F(StopBotHandlerTest, KnownBotStopsOwnedStream) {
    InsertBot("bot-1", "stream-1");
    ASSERT_EQ(streams_->Size(), 1u);

    open_switch::control::v1::StopBotRequest req;
    req.set_bot_id("bot-1");
    open_switch::control::v1::StopBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopBot(&ctx, &req, &resp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_TRUE(resp.was_active());
    EXPECT_EQ(streams_->Size(), 0u);
    EXPECT_EQ(bots_->ActiveCount(), 0u);
}

TEST_F(StopBotHandlerTest, SecondStopIsIdempotent) {
    InsertBot("bot-1", "stream-1");

    open_switch::control::v1::StopBotRequest req;
    req.set_bot_id("bot-1");
    grpc::ServerContext ctx;
    open_switch::control::v1::StopBotResponse resp1;
    ASSERT_TRUE(svc_->StopBot(&ctx, &req, &resp1).ok());
    ASSERT_TRUE(resp1.was_active());

    open_switch::control::v1::StopBotResponse resp2;
    const grpc::Status st = svc_->StopBot(&ctx, &req, &resp2);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp2.was_active());
}

}  // namespace
