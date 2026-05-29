/*
 * tests/unit/control/start_recording_relay_handler_test.cc
 *
 * Unit tests for StartRecordingRelay via ControlServiceSkeleton.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1) plus an in-process
 * MediaBridge service so StreamClient::Open() completes without a
 * production test seam.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.grpc.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/bug_handle.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

constexpr const char* kChannelUuid = "recording-channel-0001";
constexpr const char* kTenantId = "tenant-a";
switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0x7B01);
switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0x7B02);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0x7B03);

using CapturedEvent = osw::raii::fs::MockState::CapturedEvent;

std::optional<CapturedEvent> CapturedEventCopy(switch_event_t* ptr) {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(ptr);
    if (it == m.events_by_ptr.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool HasHeader(const CapturedEvent& event, const std::string& name, const std::string& value) {
    return std::any_of(event.headers.begin(), event.headers.end(), [&](const auto& kv) {
        return kv.first == name && kv.second == value;
    });
}

class RecordingSinkService final : public open_switch::media::v1::MediaBridge::Service {
  public:
    grpc::Status Stream(
        grpc::ServerContext* /*ctx*/,
        grpc::ServerReaderWriter<open_switch::media::v1::FromService,
                                 open_switch::media::v1::FromModule>* stream) override {
        open_switch::media::v1::FromModule first;
        if (!stream->Read(&first) || !first.has_start()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected StreamStart");
        }

        {
            std::lock_guard<std::mutex> g(mu_);
            starts_.push_back(first.start());
        }

        open_switch::media::v1::FromService ready_msg;
        auto* ready = ready_msg.mutable_ready();
        ready->set_sample_rate_hz(first.start().sample_rate_hz());
        ready->set_channels(first.start().channels());
        ready->set_codec(first.start().codec());
        ready->set_server_stream_id("recording-sink-test-stream");
        if (!stream->Write(ready_msg)) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "failed to write StreamReady");
        }

        open_switch::media::v1::FromModule msg;
        while (stream->Read(&msg)) {
        }
        return grpc::Status::OK;
    }

    std::vector<open_switch::media::v1::StreamStart> Starts() const {
        std::lock_guard<std::mutex> g(mu_);
        return starts_;
    }

  private:
    mutable std::mutex mu_;
    std::vector<open_switch::media::v1::StreamStart> starts_;
};

class StartRecordingRelayHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        auto& m = osw::raii::fs::Mock();
        m.next_session = kFakeSession;
        m.next_bleg_uuid = kChannelUuid;
        m.next_bug = kFakeBug;
        m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
        m.next_event = kAuditEvent;

        sink_ = std::make_unique<RecordingSinkService>();
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &sink_port_);
        builder.RegisterService(sink_.get());
        sink_server_ = builder.BuildAndStart();
        ASSERT_NE(sink_server_, nullptr);
        ASSERT_GT(sink_port_, 0);

        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
        bug_mgr_ = std::make_unique<osw::media::MediaBugManager>();
        streams_ = std::make_unique<osw::control::ActiveMediaStreams>();
        svc_->SetMediaBugManager(bug_mgr_.get());
        svc_->SetActiveMediaStreams(streams_.get());
        svc_->SetConfig(&config_);
    }

    void TearDown() override {
        inject_bugs_.clear();
        streams_.reset();
        bug_mgr_.reset();
        svc_.reset();
        health_.reset();
        if (sink_server_) {
            sink_server_->Shutdown(std::chrono::system_clock::now() +
                                   std::chrono::milliseconds(500));
        }
        sink_server_.reset();
        sink_.reset();
        osw::raii::fs::MockReset();
    }

    std::string Endpoint() const { return "127.0.0.1:" + std::to_string(sink_port_); }

    void PrimeInjectBug() {
        osw::media::BugConfig cfg;
        cfg.purpose = osw::media::Purpose::kTtsPlayback;
        cfg.fs_flags = SMBF_WRITE_REPLACE;
        cfg.target_rate_hz = 16000;
        cfg.tenant_id = kTenantId;
        cfg.stream_endpoint = "tts-upstream";

        auto attached = bug_mgr_->Attach(kFakeSession, std::move(cfg));
        ASSERT_TRUE(attached.ok) << attached.error;
        ASSERT_TRUE(bug_mgr_->HasInjectBug(kChannelUuid));
        inject_bugs_.push_back(std::move(attached.handle));
    }

    open_switch::control::v1::StartRecordingRelayRequest BaseRequest() const {
        open_switch::control::v1::StartRecordingRelayRequest req;
        req.mutable_header()->set_tenant_id(kTenantId);
        req.mutable_header()->set_traceparent(
            "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01");
        req.set_channel_uuid(kChannelUuid);
        req.set_relay_endpoint(Endpoint());
        return req;
    }

    std::unique_ptr<RecordingSinkService> sink_;
    std::unique_ptr<grpc::Server> sink_server_;
    int sink_port_ = 0;

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    osw::Config config_;
    std::vector<osw::media::BugHandle> inject_bugs_;
};

TEST_F(StartRecordingRelayHandlerTest, EmptyChannelUuidReturnsInvalidArgument) {
    auto req = BaseRequest();
    req.clear_channel_uuid();
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartRecordingRelayHandlerTest, EmptyRelayEndpointReturnsInvalidArgument) {
    auto req = BaseRequest();
    req.clear_relay_endpoint();
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartRecordingRelayHandlerTest, InvalidSampleRateReturnsInvalidArgument) {
    auto req = BaseRequest();
    req.set_sample_rate_hz(44100);
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StartRecordingRelayHandlerTest, NullMediaPlaneReturnsUnavailable) {
    auto svc = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    svc->SetConfig(&config_);

    auto req = BaseRequest();
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(StartRecordingRelayHandlerTest, ChannelNotFoundReturnsNotFound) {
    osw::raii::fs::Mock().next_session = nullptr;

    auto req = BaseRequest();
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(StartRecordingRelayHandlerTest, NoInjectBugReturnsFailedPrecondition) {
    auto req = BaseRequest();
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(streams_->Size(), 0u);
    EXPECT_EQ(osw::raii::fs::Mock().media_bug_add_calls.load(), 0);
    EXPECT_TRUE(sink_->Starts().empty());
}

TEST_F(StartRecordingRelayHandlerTest, StereoHappyPathAttachesReadWriteAndAudits) {
    PrimeInjectBug();
    const int bug_adds_before = osw::raii::fs::Mock().media_bug_add_calls.load();

    auto req = BaseRequest();
    req.set_stereo(true);
    req.set_sample_rate_hz(16000);
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.stream_id().empty());
    EXPECT_EQ(resp.negotiated_rate_hz(), 16000u);
    EXPECT_EQ(streams_->Size(), 1u);

    const auto starts = sink_->Starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0].channel_uuid(), kChannelUuid);
    EXPECT_EQ(starts[0].tenant_id(), kTenantId);
    EXPECT_EQ(starts[0].purpose(), open_switch::media::v1::StreamStart::RECORDING_RELAY);
    EXPECT_EQ(starts[0].sample_rate_hz(), 16000u);
    EXPECT_EQ(starts[0].channels(), 2u);
    EXPECT_EQ(starts[0].side(), open_switch::media::v1::StreamStart::STEREO);
    EXPECT_EQ(starts[0].traceparent(), "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.media_bug_add_calls.load(), bug_adds_before + 2);
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        ASSERT_GE(m.media_bug_add_invocations.size(), 3u);
        const auto& read_bug = m.media_bug_add_invocations[m.media_bug_add_invocations.size() - 2];
        const auto& write_bug = m.media_bug_add_invocations.back();
        EXPECT_EQ(read_bug.target, "recording_relay");
        EXPECT_NE(read_bug.flags & SMBF_READ_STREAM, 0u);
        EXPECT_EQ(write_bug.target, "recording_relay");
        EXPECT_NE(write_bug.flags & SMBF_WRITE_STREAM, 0u);
    }

    ASSERT_EQ(m.event_create_subclass_calls.load(), 1);
    auto event = CapturedEventCopy(kAuditEvent);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->subclass_name, "osw.recording.relay_started");
    EXPECT_TRUE(HasHeader(*event, "channel_uuid", kChannelUuid));
    EXPECT_TRUE(HasHeader(*event, "stream_id", resp.stream_id()));
    EXPECT_TRUE(HasHeader(*event, "tenant_id", kTenantId));
    EXPECT_TRUE(HasHeader(*event, "stereo", "true"));
}

TEST_F(StartRecordingRelayHandlerTest, MonoHappyPathUsesDefaultRateAndMixedSide) {
    config_.recording_default_rate_hz = 24000;
    PrimeInjectBug();

    auto req = BaseRequest();
    req.set_stereo(false);
    open_switch::control::v1::StartRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartRecordingRelay(&ctx, &req, &resp);

    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.stream_id().empty());
    EXPECT_EQ(resp.negotiated_rate_hz(), 24000u);
    EXPECT_EQ(streams_->Size(), 1u);

    const auto starts = sink_->Starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0].sample_rate_hz(), 24000u);
    EXPECT_EQ(starts[0].channels(), 1u);
    EXPECT_EQ(starts[0].side(), open_switch::media::v1::StreamStart::BOTH_MIXED);
}

}  // namespace
