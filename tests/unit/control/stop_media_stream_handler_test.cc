/*
 * tests/unit/control/stop_media_stream_handler_test.cc
 *
 * Unit tests for HandleStopMediaStream via ControlServiceSkeleton.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1) and
 * osw_control_w6c_test_helpers.
 *
 * Acceptance scenarios:
 *   S1 — Null bug_mgr / streams → UNAVAILABLE.
 *   S2 — Empty stream_id → INVALID_ARGUMENT.
 *   S3 — Unknown stream_id → OK + was_active=false.
 *   S4 — Known stream_id → OK + was_active=true.
 *   S5 — Remove is idempotent: second call → OK + was_active=false.
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

class StopMediaStreamHandlerTest : public ::testing::Test {
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

    /// Insert a minimal stream with the given stream_id.
    void InsertStream(const std::string& stream_id) {
        auto s = std::make_unique<osw::control::ActiveMediaStream>();
        s->channel_uuid = "chan-uuid";
        s->stream_id = stream_id;
        s->purpose = open_switch::media::v1::StreamStart::STT_TRANSCRIBE;
        (void)streams_->Insert(std::move(s));
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    osw::Config config_;
};

// ---------------------------------------------------------------------------
// S1 — No streams injected → UNAVAILABLE
// ---------------------------------------------------------------------------

TEST_F(StopMediaStreamHandlerTest, NullStreamsReturnsUnavailable) {
    auto svc2 = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    // Do NOT call SetActiveMediaStreams → streams pointer is null.

    open_switch::control::v1::StopMediaStreamRequest req;
    req.set_stream_id("some-id");
    open_switch::control::v1::StopMediaStreamResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc2->StopMediaStream(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// S2 — Empty stream_id → INVALID_ARGUMENT
// ---------------------------------------------------------------------------

TEST_F(StopMediaStreamHandlerTest, EmptyStreamIdReturnsInvalidArgument) {
    open_switch::control::v1::StopMediaStreamRequest req;
    // stream_id left empty
    open_switch::control::v1::StopMediaStreamResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopMediaStream(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// S3 — Unknown stream_id → OK + was_active=false
// ---------------------------------------------------------------------------

TEST_F(StopMediaStreamHandlerTest, UnknownStreamIdReturnsOkWasActiveFalse) {
    open_switch::control::v1::StopMediaStreamRequest req;
    req.set_stream_id("nonexistent-id");
    open_switch::control::v1::StopMediaStreamResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopMediaStream(&ctx, &req, &resp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_FALSE(resp.was_active());
}

// ---------------------------------------------------------------------------
// S4 — Known stream_id → OK + was_active=true
// ---------------------------------------------------------------------------

TEST_F(StopMediaStreamHandlerTest, KnownStreamIdReturnsOkWasActiveTrue) {
    InsertStream("stream-abc");
    ASSERT_EQ(streams_->Size(), 1u);

    open_switch::control::v1::StopMediaStreamRequest req;
    req.set_stream_id("stream-abc");
    open_switch::control::v1::StopMediaStreamResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status st = svc_->StopMediaStream(&ctx, &req, &resp);
    EXPECT_TRUE(st.ok()) << st.error_message();
    EXPECT_TRUE(resp.was_active());
    EXPECT_EQ(streams_->Size(), 0u);
}

// ---------------------------------------------------------------------------
// S5 — Idempotent: second call returns was_active=false
// ---------------------------------------------------------------------------

TEST_F(StopMediaStreamHandlerTest, SecondStopIsIdempotent) {
    InsertStream("stream-xyz");

    grpc::ServerContext ctx;
    open_switch::control::v1::StopMediaStreamResponse resp;

    open_switch::control::v1::StopMediaStreamRequest req;
    req.set_stream_id("stream-xyz");

    svc_->StopMediaStream(&ctx, &req, &resp);
    ASSERT_TRUE(resp.was_active());

    open_switch::control::v1::StopMediaStreamResponse resp2;
    const grpc::Status st2 = svc_->StopMediaStream(&ctx, &req, &resp2);
    EXPECT_TRUE(st2.ok()) << st2.error_message();
    EXPECT_FALSE(resp2.was_active());
}

}  // namespace
