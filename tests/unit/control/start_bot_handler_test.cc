/*
 * tests/unit/control/start_bot_handler_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/bug_manager.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

class StartBotHandlerTest : public ::testing::Test {
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

    open_switch::control::v1::StartBotRequest ValidRequest() const {
        open_switch::control::v1::StartBotRequest req;
        req.add_target_channel_uuids("chan-a");
        req.set_upstream_endpoint("localhost:50051");
        req.set_purpose(open_switch::control::v1::StartBotRequest::TTS_BROADCAST);
        return req;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    std::unique_ptr<osw::control::ActiveBots> bots_;
    osw::Config config_;
};

TEST_F(StartBotHandlerTest, NullMediaPlaneReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    svc2->SetConfig(&config_);

    auto req = ValidRequest();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(StartBotHandlerTest, NullConfigReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    svc2->SetMediaBugManager(bug_mgr_.get());
    svc2->SetActiveMediaStreams(streams_.get());
    svc2->SetActiveBots(bots_.get());

    auto req = ValidRequest();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(StartBotHandlerTest, EmptyTargetsReturnsInvalidArgument) {
    auto req = ValidRequest();
    req.clear_target_channel_uuids();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, TooManyTargetsReturnsResourceExhausted) {
    auto req = ValidRequest();
    req.add_target_channel_uuids("chan-b");
    req.add_target_channel_uuids("chan-c");
    config_.bot_max_targets = 2;
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST_F(StartBotHandlerTest, DuplicateTargetReturnsInvalidArgument) {
    auto req = ValidRequest();
    req.add_target_channel_uuids("chan-a");
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, MissingEndpointReturnsInvalidArgument) {
    auto req = ValidRequest();
    req.clear_upstream_endpoint();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, MissingPurposeReturnsInvalidArgument) {
    auto req = ValidRequest();
    req.set_purpose(open_switch::control::v1::StartBotRequest::PURPOSE_UNSPECIFIED);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, SttListenReturnsUnimplementedForNow) {
    auto req = ValidRequest();
    req.set_purpose(open_switch::control::v1::StartBotRequest::STT_LISTEN);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST_F(StartBotHandlerTest, InvalidSampleRateReturnsInvalidArgument) {
    auto req = ValidRequest();
    req.set_sample_rate_hz(48000);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, WriteTargetsRejectedForNonWhisper) {
    auto req = ValidRequest();
    req.add_write_target_channel_uuids("chan-a");
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartBotHandlerTest, DrainTimeoutOverrideReturnsUnimplemented) {
    auto req = ValidRequest();
    req.set_drain_timeout_ms(500);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST_F(StartBotHandlerTest, ChannelCapacityReturnsFailedPrecondition) {
    osw::control::ActiveBot bot;
    bot.bot_id = "bot-1";
    bot.target_channel_uuids = {"chan-a"};
    bot.stream_ids = {"stream-a"};
    ASSERT_TRUE(bots_->Insert(std::move(bot)));

    auto req = ValidRequest();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

}  // namespace
