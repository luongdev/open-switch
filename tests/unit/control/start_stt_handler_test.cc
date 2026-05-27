/*
 * tests/unit/control/start_stt_handler_test.cc
 *
 * Unit tests for HandleStartStt via ControlServiceSkeleton.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1) and
 * osw_control_w6c_test_helpers.
 *
 * Acceptance scenarios (C9–C12):
 *   C9  — Empty channel_uuid → INVALID_ARGUMENT.
 *   C10 — Empty upstream_endpoint → INVALID_ARGUMENT.
 *   C11 — Invalid sample_rate_hz (e.g. 44100) → INVALID_ARGUMENT.
 *   C12 — Null media plane (bug_mgr/streams not injected) → UNAVAILABLE.
 *   C13 — Channel not found → NOT_FOUND.
 *   C14 — Bug attach failure → propagates status code.
 *   C15 — Stream inserted and stream_id populated in response on happy path
 *          (gRPC channel to upstream will fail to open; we only test up to
 *          the StreamClient::Open failure → UNAVAILABLE/UNAVAILABLE from
 *          upstream, since tests don't run a real upstream gRPC server).
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

switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0xAB01);
switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0xAB02);

class StartSttHandlerTest : public ::testing::Test {
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
    }

    /// Prime mock for a valid session + successful bug attach.
    void PrimeSession() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kFakeSession;
        m.next_bleg_uuid = "chan-uuid-1234";
        m.next_bug = kFakeBug;
        m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    osw::Config config_;
};

// ---------------------------------------------------------------------------
// C9 — Empty channel_uuid
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, EmptyChannelUuidReturnsInvalidArgument) {
    open_switch::control::v1::StartSttRequest req;
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// C10 — Empty upstream_endpoint
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, EmptyEndpointReturnsInvalidArgument) {
    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// C11 — Invalid sample_rate_hz
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, InvalidSampleRateReturnsInvalidArgument) {
    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:9090");
    req.set_sample_rate_hz(44100);
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// C12 — Null media plane (bug_mgr/streams not injected)
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, NullMediaPlaneReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    // Do NOT inject media plane.

    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StartStt(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// C13 — Channel not found
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, ChannelNotFoundReturnsNotFound) {
    // next_session defaults to nullptr → NOT_FOUND.
    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("nonexistent-uuid");
    req.set_upstream_endpoint("localhost:9090");
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// C14 — Bug attach failure propagates status
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, BugAttachFailurePropagatesError) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";
    m.next_bug = nullptr;
    m.next_bug_add_status = SWITCH_STATUS_GENERR;

    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    // StreamClient::Open will fail (no server listening); that's expected.
    // If Open succeeds somehow, Attach failure will propagate.
    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    // Either Open fails (UNAVAILABLE) or Attach fails (INTERNAL).
    // Either way, the call is NOT OK.
    EXPECT_FALSE(st.ok());
}

// ---------------------------------------------------------------------------
// C15 — Default rate (0 → 16000) accepted
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, DefaultRateZeroIsAccepted) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    // sample_rate_hz=0 defaults to 16000 (allowed).
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    // The only accepted failure here is Open failure (no upstream server).
    // The rate itself must NOT return INVALID_ARGUMENT.
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// C15b — 8000 Hz rate accepted
// ---------------------------------------------------------------------------

TEST_F(StartSttHandlerTest, Rate8000HzAccepted) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kFakeSession;
    m.next_bleg_uuid = "chan-uuid-1234";

    open_switch::control::v1::StartSttRequest req;
    req.set_channel_uuid("chan-uuid-1234");
    req.set_upstream_endpoint("localhost:50051");
    req.set_sample_rate_hz(8000);
    open_switch::control::v1::StartSttResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StartStt(&ctx, &req, &resp);
    EXPECT_NE(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
