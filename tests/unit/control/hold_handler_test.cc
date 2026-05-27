/*
 * tests/unit/control/hold_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::Hold against the FS-mock seam.
 *
 * Covered:
 *   - Happy path: channel answered, not already held → OK + audit emitted.
 *   - Empty uuids list → INVALID_ARGUMENT (no locate call).
 *   - Empty uuid string in list → INVALID_ARGUMENT.
 *   - UUID not found → NOT_FOUND.
 *   - Channel not answered (CF_ANSWERED not set) → FAILED_PRECONDITION.
 *   - Channel already on hold (CF_HOLD set) → FAILED_PRECONDITION.
 *   - HoldUuid FS call made with correct uuid and moh=SWITCH_TRUE.
 *   - Audit emitted once on happy path.
 *   - Audit NOT emitted on failure paths.
 *   - Session locked and unlocked.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/observability/health.h"
#include "osw/raii/fs_mock.h"

#include "src/control/control_service_skeleton.h"

namespace {

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0xCB01);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xCB02);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xCB03);

class HoldHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    // Prime mock for an answered channel, not on hold.
    void PrimeAnsweredChannel() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kSession;
        m.next_channel = kChannel;
        // CF_ANSWERED = 1, CF_HOLD = 4; answered but not on hold.
        m.next_channel_flags = CF_ANSWERED;
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, HappyPathReturnsOK) {
    PrimeAnsweredChannel();

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(HoldHandlerTest, HappyPathHeldUuidInResponse) {
    PrimeAnsweredChannel();

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    svc_->Hold(&ctx, &req, &resp);

    ASSERT_EQ(resp.held_uuids_size(), 1);
    EXPECT_EQ(resp.held_uuids(0), "uuid-1234");
}

TEST_F(HoldHandlerTest, HappyPathCallsHoldUuidWithCorrectArgs) {
    PrimeAnsweredChannel();

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    svc_->Hold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.hold_uuid_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.hold_uuid_invocations.size(), 1u);
    EXPECT_EQ(m.hold_uuid_invocations[0].uuid, "uuid-1234");
    EXPECT_EQ(m.hold_uuid_invocations[0].message, "");  // nullptr → empty in mock
    EXPECT_EQ(m.hold_uuid_invocations[0].moh, SWITCH_TRUE);
}

TEST_F(HoldHandlerTest, HappyPathAuditEmitted) {
    PrimeAnsweredChannel();

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    svc_->Hold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(HoldHandlerTest, SessionLockedAndUnlocked) {
    PrimeAnsweredChannel();

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    svc_->Hold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT — empty uuids list
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, EmptyUuidsListReturnsInvalidArgument) {
    open_switch::control::v1::HoldRequest req;
    // No uuids added.
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.hold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(HoldHandlerTest, EmptyUuidStringReturnsInvalidArgument) {
    open_switch::control::v1::HoldRequest req;
    req.add_uuids("");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session defaults to nullptr → not found.
    open_switch::control::v1::HoldRequest req;
    req.add_uuids("nonexistent-uuid");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.hold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION — channel not answered
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, ChannelNotAnsweredReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    // No flags set → CF_ANSWERED not present.
    m.next_channel_flags = 0;

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.hold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION — channel already on hold
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, ChannelAlreadyOnHoldReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    // Both CF_ANSWERED and CF_HOLD set.
    m.next_channel_flags = CF_ANSWERED | CF_HOLD;

    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.hold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Audit NOT emitted on failure paths
// ---------------------------------------------------------------------------

TEST_F(HoldHandlerTest, AuditNotEmittedOnNotFound) {
    // next_session = nullptr → NOT_FOUND.
    open_switch::control::v1::HoldRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::HoldResponse resp;
    grpc::ServerContext ctx;

    svc_->Hold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

}  // namespace
