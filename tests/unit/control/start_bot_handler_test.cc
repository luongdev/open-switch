/*
 * tests/unit/control/start_bot_handler_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.grpc.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/bug_manager.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0xE001);
switch_channel_t* const kFakeChannel = reinterpret_cast<switch_channel_t*>(0xE002);
switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0xE003);

class MockMediaBridgeService final : public open_switch::media::v1::MediaBridge::Service {
  public:
    std::atomic<int> stream_count{0};

    grpc::Status Stream(
        grpc::ServerContext*,
        grpc::ServerReaderWriter<open_switch::media::v1::FromService,
                                 open_switch::media::v1::FromModule>* stream) override {
        stream_count.fetch_add(1, std::memory_order_relaxed);

        open_switch::media::v1::FromModule req;
        if (!stream->Read(&req) || !req.has_start()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected StreamStart");
        }

        open_switch::media::v1::FromService ready_msg;
        auto* ready = ready_msg.mutable_ready();
        ready->set_sample_rate_hz(req.start().sample_rate_hz());
        ready->set_channels(req.start().channels());
        ready->set_codec(req.start().codec());
        ready->set_server_stream_id("start-bot-test");
        if (!stream->Write(ready_msg)) {
            return grpc::Status::OK;
        }

        open_switch::media::v1::FromModule ignored;
        while (stream->Read(&ignored)) {
        }
        return grpc::Status::OK;
    }
};

class StartBotHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        media_svc_ = std::make_unique<MockMediaBridgeService>();
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port_);
        builder.RegisterService(media_svc_.get());
        media_server_ = builder.BuildAndStart();
        ASSERT_NE(media_server_, nullptr);
        ASSERT_GT(bound_port_, 0);

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

    void TearDown() override {
        if (media_server_) {
            media_server_->Shutdown(std::chrono::system_clock::now() +
                                    std::chrono::milliseconds(500));
        }
    }

    open_switch::control::v1::StartBotRequest ValidRequest() const {
        open_switch::control::v1::StartBotRequest req;
        req.add_target_channel_uuids("chan-a");
        req.set_upstream_endpoint("127.0.0.1:" + std::to_string(bound_port_));
        req.set_purpose(open_switch::control::v1::StartBotRequest::TTS_BROADCAST);
        return req;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    std::unique_ptr<osw::control::ActiveBots> bots_;
    osw::Config config_;
    std::unique_ptr<MockMediaBridgeService> media_svc_;
    std::unique_ptr<grpc::Server> media_server_;
    int bound_port_ = 0;
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

TEST_F(StartBotHandlerTest, SttListenUsesOneUpstreamStreamAndOneReadBug) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_channel = kFakeChannel;
    m.next_bug = kFakeBug;
    m.next_bug_add_status = SWITCH_STATUS_SUCCESS;

    auto req = ValidRequest();
    req.set_purpose(open_switch::control::v1::StartBotRequest::STT_LISTEN);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(media_svc_->stream_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(m.media_bug_add_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-a"), 1u);
    EXPECT_EQ(bots_->ActiveCount(), 1u);
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

TEST_F(StartBotHandlerTest, DrainTimeoutOverrideDoesNotReturnUnimplemented) {
    auto req = ValidRequest();
    req.set_drain_timeout_ms(500);
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_NE(st.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST_F(StartBotHandlerTest, MissingTargetIsRejectedByAuthoritativeAttach) {
    auto req = ValidRequest();
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_EQ(bug_mgr_->TotalActiveBugCount(), 0u);
    EXPECT_EQ(bots_->ActiveCount(), 0u);
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

TEST_F(StartBotHandlerTest, TtsBroadcastTwoTargetsUsesOneUpstreamStreamAndTwoBugs) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_channel = kFakeChannel;
    m.next_bug = kFakeBug;
    m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
    config_.bot_max_targets = 2;

    auto req = ValidRequest();
    req.add_target_channel_uuids("chan-b");
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.bot_id().empty());
    EXPECT_EQ(resp.negotiated_rate_hz(), 16000u);

    EXPECT_EQ(media_svc_->stream_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(m.media_bug_add_calls.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-a"), 1u);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-b"), 1u);
    EXPECT_EQ(bots_->ActiveCount(), 1u);
    EXPECT_EQ(streams_->Size(), 0u);
}

TEST_F(StartBotHandlerTest, VoicebotDuplexTwoTargetsAttachesReadAndWriteBugs) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_channel = kFakeChannel;
    m.next_bug = kFakeBug;
    m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
    config_.bot_max_targets = 2;

    auto req = ValidRequest();
    req.set_purpose(open_switch::control::v1::StartBotRequest::VOICEBOT_DUPLEX);
    req.add_target_channel_uuids("chan-b");
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();

    EXPECT_EQ(media_svc_->stream_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(m.media_bug_add_calls.load(std::memory_order_relaxed), 4);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-a"), 2u);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-b"), 2u);
    EXPECT_EQ(bots_->ActiveCount(), 1u);
}

TEST_F(StartBotHandlerTest, WhisperTwoTargetsAttachesReadAllAndWriteSubset) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_channel = kFakeChannel;
    m.next_bug = kFakeBug;
    m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
    config_.bot_max_targets = 2;

    auto req = ValidRequest();
    req.set_purpose(open_switch::control::v1::StartBotRequest::WHISPER);
    req.add_target_channel_uuids("chan-b");
    req.add_write_target_channel_uuids("chan-b");
    open_switch::control::v1::StartBotResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartBot(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();

    EXPECT_EQ(media_svc_->stream_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(m.media_bug_add_calls.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-a"), 1u);
    EXPECT_EQ(bug_mgr_->ActiveBugCount("chan-b"), 2u);
    EXPECT_EQ(bots_->ActiveCount(), 1u);
}

}  // namespace
