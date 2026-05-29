/*
 * tests/unit/control/stop_recording_relay_handler_test.cc
 *
 * Unit tests for StopRecordingRelay via ControlServiceSkeleton.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1) and the shared
 * ActiveMediaStreams teardown path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/recording_relay.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

constexpr const char* kTenantId = "tenant-a";
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0x7C01);

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

class StopRecordingRelayHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        osw::raii::fs::Mock().next_event = kAuditEvent;

        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
        streams_ = std::make_unique<osw::control::ActiveMediaStreams>();
        svc_->SetActiveMediaStreams(streams_.get());
        svc_->SetConfig(&config_);
    }

    void TearDown() override {
        streams_.reset();
        svc_.reset();
        health_.reset();
        osw::raii::fs::MockReset();
    }

    void InsertStream(
        const std::string& stream_id,
        const std::string& channel_uuid,
        open_switch::media::v1::StreamStart::Purpose purpose =
            open_switch::media::v1::StreamStart::RECORDING_RELAY,
        bool with_recording_ctx = false) {
        auto stream = std::make_unique<osw::control::ActiveMediaStream>();
        stream->channel_uuid = channel_uuid;
        stream->stream_id = stream_id;
        stream->purpose = purpose;
        if (with_recording_ctx) {
            osw::media::RecordingRelayConfig cfg;
            cfg.channel_uuid = channel_uuid;
            cfg.tenant_id = kTenantId;
            cfg.stream_id = stream_id;
            stream->recording_ctx = std::unique_ptr<void, osw::control::RecordingCtxDeleter>(
                new osw::media::RecordingRelay(nullptr, std::move(cfg)));
        }
        ASSERT_TRUE(streams_->Insert(std::move(stream)));
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    osw::Config config_;
};

TEST_F(StopRecordingRelayHandlerTest, NullStreamsReturnsUnavailable) {
    auto svc = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    svc->SetConfig(&config_);

    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_stream_id("stream-a");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc->StopRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(StopRecordingRelayHandlerTest, EmptySelectorReturnsInvalidArgument) {
    open_switch::control::v1::StopRecordingRelayRequest req;
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);

    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(StopRecordingRelayHandlerTest, UnknownStreamIdReturnsOkZeroStopped) {
    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_stream_id("missing-stream");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);

    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.streams_stopped(), 0u);
}

TEST_F(StopRecordingRelayHandlerTest, KnownStreamIdStopsOneAndIsIdempotent) {
    InsertStream("stream-a", "chan-a");
    ASSERT_EQ(streams_->Size(), 1u);

    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_stream_id("stream-a");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);
    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.streams_stopped(), 1u);
    EXPECT_EQ(streams_->Size(), 0u);

    open_switch::control::v1::StopRecordingRelayResponse resp2;
    const grpc::Status st2 = svc_->StopRecordingRelay(&ctx, &req, &resp2);
    EXPECT_TRUE(st2.ok()) << st2.error_message();
    EXPECT_EQ(resp2.streams_stopped(), 0u);
}

TEST_F(StopRecordingRelayHandlerTest, StreamIdWithWrongPurposeStopsZero) {
    InsertStream("stream-a", "chan-a", open_switch::media::v1::StreamStart::TTS_PLAYBACK);

    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_stream_id("stream-a");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);

    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.streams_stopped(), 0u);
    EXPECT_EQ(streams_->Size(), 1u);
}

TEST_F(StopRecordingRelayHandlerTest, ChannelUuidStopsOnlyRecordingRelaysForChannel) {
    InsertStream("rec-a", "chan-a");
    InsertStream("rec-b", "chan-a");
    InsertStream("tts-a", "chan-a", open_switch::media::v1::StreamStart::TTS_PLAYBACK);
    InsertStream("rec-c", "chan-b");
    ASSERT_EQ(streams_->Size(), 4u);

    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_channel_uuid("chan-a");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);

    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.streams_stopped(), 2u);
    EXPECT_EQ(streams_->Size(), 2u);
}

TEST_F(StopRecordingRelayHandlerTest, StopWithRecordingContextEmitsRelayStoppedAudit) {
    InsertStream("stream-a", "chan-a", open_switch::media::v1::StreamStart::RECORDING_RELAY,
                 true);

    open_switch::control::v1::StopRecordingRelayRequest req;
    req.set_stream_id("stream-a");
    open_switch::control::v1::StopRecordingRelayResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopRecordingRelay(&ctx, &req, &resp);

    ASSERT_TRUE(st.ok()) << st.error_message();
    EXPECT_EQ(resp.streams_stopped(), 1u);
    EXPECT_EQ(osw::raii::fs::Mock().event_create_subclass_calls.load(), 1);

    auto event = CapturedEventCopy(kAuditEvent);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->subclass_name, "osw.recording.relay_stopped");
    EXPECT_TRUE(HasHeader(*event, "channel_uuid", "chan-a"));
    EXPECT_TRUE(HasHeader(*event, "stream_id", "stream-a"));
    EXPECT_TRUE(HasHeader(*event, "tenant_id", kTenantId));
}

}  // namespace
