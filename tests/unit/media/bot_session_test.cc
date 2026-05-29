/*
 * tests/unit/media/bot_session_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "osw/media/bot_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "open_switch/media/v1/media.grpc.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/media/audio_frame.h"
#include "osw/media/bug_manager.h"

namespace {

using open_switch::control::v1::StartBotRequest;
using osw::media::BotSession;
using osw::media::BotSessionConfig;

switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0xD001);
switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0xD002);

extern "C" switch_bool_t DummyBotCallback(switch_media_bug_t*, void*, switch_abc_type_t) noexcept {
    return SWITCH_TRUE;
}

class MockMediaBridgeService final : public open_switch::media::v1::MediaBridge::Service {
  public:
    std::atomic<int> stream_count{0};
    int send_audio_count = 0;
    std::mutex received_mu;
    std::vector<open_switch::media::v1::AudioFrame> received_audio;

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
        ready->set_server_stream_id("bot-session-test");
        if (!stream->Write(ready_msg)) {
            return grpc::Status::OK;
        }

        for (int i = 0; i < send_audio_count; ++i) {
            open_switch::media::v1::FromService audio_msg;
            auto* audio = audio_msg.mutable_audio();
            audio->set_seq(static_cast<std::uint64_t>(i));
            audio->set_timestamp_samples(static_cast<std::uint64_t>(i) * 160);
            audio->set_duration_samples(160);
            audio->set_payload(std::string(320, static_cast<char>(i + 1)));
            if (!stream->Write(audio_msg)) {
                break;
            }
        }

        open_switch::media::v1::FromModule ignored;
        while (stream->Read(&ignored)) {
            if (ignored.has_audio()) {
                std::lock_guard<std::mutex> lock(received_mu);
                received_audio.push_back(ignored.audio());
            }
        }
        return grpc::Status::OK;
    }
};

class BotSessionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        svc_ = std::make_unique<MockMediaBridgeService>();
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port_);
        builder.RegisterService(svc_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_GT(bound_port_, 0);
    }

    void TearDown() override {
        if (server_) {
            server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
        }
        osw::raii::fs::MockReset();
    }

    std::shared_ptr<grpc::Channel> MakeChannel() const {
        return grpc::CreateChannel("127.0.0.1:" + std::to_string(bound_port_),
                                   grpc::InsecureChannelCredentials());
    }

    static BotSessionConfig MakeConfig() {
        BotSessionConfig cfg;
        cfg.bot_id = "bot-1";
        cfg.tenant_id = "tenant-1";
        cfg.upstream_endpoint = "unused";
        cfg.target_channel_uuids = {"chan-a", "chan-b"};
        cfg.purpose = StartBotRequest::TTS_BROADCAST;
        cfg.sample_rate_hz = 8000;
        cfg.target_queue_ms = 100;
        return cfg;
    }

    std::unique_ptr<MockMediaBridgeService> svc_;
    std::unique_ptr<grpc::Server> server_;
    int bound_port_ = 0;
};

TEST_F(BotSessionTest, OpenFansOutServiceAudioToBothTargets) {
    svc_->send_audio_count = 1;

    BotSession bot(MakeConfig(), MakeChannel());
    ASSERT_TRUE(bot.Open(1000).ok());

    for (int i = 0; i < 50 && bot.FramesReceivedFromUpstream() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(bot.FramesReceivedFromUpstream(), 1u);

    auto a = bot.PopWriteFrame("chan-a");
    auto b = bot.PopWriteFrame("chan-b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->sample_count(), 160u);
    EXPECT_EQ(b->sample_count(), 160u);

    bot.Stop();
    EXPECT_TRUE(bot.IsStopped());
    EXPECT_EQ(svc_->stream_count.load(std::memory_order_relaxed), 1);
}

TEST_F(BotSessionTest, AttachTtsBroadcastAddsOneWriteBugPerTarget) {
    auto& mock = osw::raii::fs::Mock();
    mock.next_session = kFakeSession;
    mock.next_bug = kFakeBug;
    mock.next_bug_add_status = SWITCH_STATUS_SUCCESS;

    osw::media::MediaBugManager mgr;
    BotSession bot(MakeConfig(), MakeChannel());

    const grpc::Status st = bot.Attach(mgr, reinterpret_cast<void*>(DummyBotCallback), nullptr);
    ASSERT_TRUE(st.ok()) << st.error_message();

    EXPECT_EQ(mgr.ActiveBugCount("chan-a"), 1u);
    EXPECT_EQ(mgr.ActiveBugCount("chan-b"), 1u);
    EXPECT_EQ(mgr.TotalActiveBugCount(), 2u);
    EXPECT_EQ(mock.media_bug_add_calls.load(std::memory_order_relaxed), 2);
}

TEST_F(BotSessionTest, AttachPartialFailureUnwindsAlreadyAttachedBugs) {
    auto& mock = osw::raii::fs::Mock();
    mock.next_session = kFakeSession;
    mock.next_bug = kFakeBug;
    mock.next_bug_add_status = SWITCH_STATUS_SUCCESS;

    auto cfg = MakeConfig();
    cfg.target_channel_uuids = {"chan-a", "chan-a"};

    osw::media::MediaBugManager mgr;
    BotSession bot(std::move(cfg), MakeChannel());

    const grpc::Status st = bot.Attach(mgr, reinterpret_cast<void*>(DummyBotCallback), nullptr);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(mgr.TotalActiveBugCount(), 0u);
}

TEST_F(BotSessionTest, EmptyTargetQueueReturnsNulloptForPassthrough) {
    BotSession bot(MakeConfig(), MakeChannel());
    EXPECT_FALSE(bot.PopWriteFrame("chan-a").has_value());
}

TEST_F(BotSessionTest, OnTargetReadFrameTagsSourceChannelUuid) {
    auto cfg = MakeConfig();
    cfg.purpose = StartBotRequest::STT_LISTEN;

    BotSession bot(std::move(cfg), MakeChannel());
    ASSERT_TRUE(bot.Open(1000).ok());

    std::vector<std::int16_t> samples(160, 42);
    bot.OnTargetReadFrame("chan-b",
                          /*fs_timestamp_samples=*/0,
                          samples.data(),
                          samples.size(),
                          /*sample_rate_hz=*/8000,
                          /*channels=*/1);

    for (int i = 0; i < 50; ++i) {
        {
            std::lock_guard<std::mutex> lock(svc_->received_mu);
            if (!svc_->received_audio.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bot.Stop();

    std::lock_guard<std::mutex> lock(svc_->received_mu);
    ASSERT_EQ(svc_->received_audio.size(), 1u);
    EXPECT_EQ(svc_->received_audio[0].channel_uuid(), "chan-b");
    EXPECT_EQ(svc_->received_audio[0].duration_samples(), 160u);
}

}  // namespace
