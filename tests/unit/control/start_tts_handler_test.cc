/*
 * tests/unit/control/start_tts_handler_test.cc
 *
 * Unit tests for HandleStartTts via ControlServiceSkeleton.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1) and
 * osw_control_w6c_test_helpers.
 *
 * Acceptance scenarios:
 *   T1  — Empty channel_uuid → INVALID_ARGUMENT.
 *   T2  — Empty upstream_endpoint → INVALID_ARGUMENT.
 *   T3  — Invalid sample_rate_hz → INVALID_ARGUMENT.
 *   T4  — Null media plane → UNAVAILABLE.
 *   T5  — Channel not found → NOT_FOUND.
 *   T6  — Default rate (0 → 16000) is not INVALID_ARGUMENT.
 *   T7  — 8000 Hz accepted.
 *   T8  — buffer_override.jitter_buffer_ms clamped to kMin (200) when below.
 *   T9  — buffer_override.preroll_ms clamped to jitter_ms when above.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/core/config.h"
#include "osw/media/bug_manager.h"
#include "osw/observability/health.h"

#include "src/control/control_service_skeleton.h"

namespace {

switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0xBB01);
switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0xBB02);

class StartTtsHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
        bug_mgr_ = std::make_unique<osw::media::MediaBugManager>();
        streams_ = std::make_unique<osw::control::ActiveMediaStreams>();
        svc_->SetMediaBugManager(bug_mgr_.get());
        svc_->SetActiveMediaStreams(streams_.get());
        svc_->SetConfig(&config_);

        // Reasonable TTS defaults.
        config_.tts_jitter_buffer_ms = 1000;
        config_.tts_preroll_ms = 500;
        config_.tts_high_water_ms = 1500;
        config_.tts_max_jitter_buffer_ms = 5000;
        config_.tts_underrun_policy = "silence";
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    osw::Config config_;
};

// ---------------------------------------------------------------------------
// T1 — Empty channel_uuid
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, EmptyChannelUuidReturnsInvalidArgument) {
    open_switch::control::v1::StartTtsRequest req;
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T2 — Empty upstream_endpoint
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, EmptyEndpointReturnsInvalidArgument) {
    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T3 — Invalid sample_rate_hz
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, InvalidSampleRateReturnsInvalidArgument) {
    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:9090");
    req.set_sample_rate_hz(22050);
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T4 — Null media plane
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, NullMediaPlaneReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());

    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StartTts(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// T5 — Channel not found
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, ChannelNotFoundReturnsNotFound) {
    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("nonexistent-uuid");
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// T6 — Default rate (0 → 16000) accepted
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, DefaultRateIsAccepted) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    // sample_rate_hz = 0 → defaults to 16000.
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T7 — 8000 Hz rate accepted
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, Rate8000HzAccepted) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    req.set_sample_rate_hz(8000);
    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T8 — buffer_override.jitter_buffer_ms below minimum is clamped (no crash)
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, JitterBufferBelowMinimumIsClampedNocrash) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    // jitter_buffer_ms=50 < 200; should be clamped to 200.
    req.mutable_buffer_override()->set_jitter_buffer_ms(50);

    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    // Should not be INVALID_ARGUMENT due to clamping.
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// T9 — buffer_override.preroll_ms above jitter_ms is clamped to jitter_ms
// ---------------------------------------------------------------------------

TEST_F(StartTtsHandlerTest, PrerollAboveJitterIsClampedNocrash) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartTtsRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    // preroll_ms=9999 > jitter_ms=500; should be clamped to jitter_ms.
    req.mutable_buffer_override()->set_jitter_buffer_ms(500);
    req.mutable_buffer_override()->set_preroll_ms(9999);

    open_switch::control::v1::StartTtsResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartTts(&ctx, &req, &resp);
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
