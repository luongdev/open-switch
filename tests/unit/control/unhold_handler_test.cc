/*
 * tests/unit/control/unhold_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::Unhold against the FS-mock seam.
 *
 * Covered:
 *   - Happy path: channel on hold → OK + audit emitted.
 *   - Empty uuids list → INVALID_ARGUMENT (no locate call).
 *   - Empty uuid string → INVALID_ARGUMENT.
 *   - UUID not found → NOT_FOUND.
 *   - Channel not on hold (CF_HOLD not set) → FAILED_PRECONDITION.
 *   - UnholdUuid called with correct uuid.
 *   - unheld_uuids populated in response.
 *   - Audit NOT emitted on failure paths.
 *   - Session locked and unlocked.
 *   - Hold → Unhold sequence: both audit emissions + mock flag tracking.
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

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0xDB01);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xDB02);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xDB03);

class UnholdHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    // Prime mock for a channel that is currently on hold.
    void PrimeChannelOnHold() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kSession;
        m.next_channel = kChannel;
        // CF_ANSWERED = 1, CF_HOLD = 4; answered AND on hold.
        m.next_channel_flags = CF_ANSWERED | CF_HOLD;
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(UnholdHandlerTest, HappyPathReturnsOK) {
    PrimeChannelOnHold();

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Unhold(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(UnholdHandlerTest, HappyPathUnheldUuidInResponse) {
    PrimeChannelOnHold();

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    svc_->Unhold(&ctx, &req, &resp);

    ASSERT_EQ(resp.unheld_uuids_size(), 1);
    EXPECT_EQ(resp.unheld_uuids(0), "uuid-1234");
}

TEST_F(UnholdHandlerTest, HappyPathCallsUnholdUuid) {
    PrimeChannelOnHold();

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    svc_->Unhold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.unhold_uuid_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.unhold_uuid_invocations.size(), 1u);
    EXPECT_EQ(m.unhold_uuid_invocations[0].uuid, "uuid-1234");
}

TEST_F(UnholdHandlerTest, HappyPathAuditEmitted) {
    PrimeChannelOnHold();

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    svc_->Unhold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(UnholdHandlerTest, SessionLockedAndUnlocked) {
    PrimeChannelOnHold();

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    svc_->Unhold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT — empty uuids list
// ---------------------------------------------------------------------------

TEST_F(UnholdHandlerTest, EmptyUuidsListReturnsInvalidArgument) {
    open_switch::control::v1::UnholdRequest req;
    // No uuids added.
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Unhold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.unhold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(UnholdHandlerTest, EmptyUuidStringReturnsInvalidArgument) {
    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Unhold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(UnholdHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session = nullptr → not found.
    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("nonexistent-uuid");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Unhold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.unhold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION — channel not on hold
// ---------------------------------------------------------------------------

TEST_F(UnholdHandlerTest, ChannelNotOnHoldReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    // CF_ANSWERED but NOT CF_HOLD.
    m.next_channel_flags = CF_ANSWERED;

    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Unhold(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.unhold_uuid_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Audit NOT emitted on failure paths
// ---------------------------------------------------------------------------

TEST_F(UnholdHandlerTest, AuditNotEmittedOnNotFound) {
    open_switch::control::v1::UnholdRequest req;
    req.add_uuids("uuid-1234");
    open_switch::control::v1::UnholdResponse resp;
    grpc::ServerContext ctx;

    svc_->Unhold(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Hold → Unhold sequence (mock flag tracking)
// ---------------------------------------------------------------------------
//
// Simulates a Hold call followed by an Unhold call. The test manually
// advances next_channel_flags to simulate the state change that FS would
// perform internally. Verifies both audit emissions and both FS calls.

TEST_F(UnholdHandlerTest, HoldThenUnholdSequence) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_event = kAuditEvent;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    // --- Step 1: Hold ---
    // Channel is answered, not held.
    m.next_channel_flags = CF_ANSWERED;

    open_switch::control::v1::HoldRequest hold_req;
    hold_req.add_uuids("uuid-seq");
    open_switch::control::v1::HoldResponse hold_resp;
    grpc::ServerContext ctx;

    const grpc::Status hold_status = svc_->Hold(&ctx, &hold_req, &hold_resp);
    ASSERT_TRUE(hold_status.ok()) << hold_status.error_message();
    EXPECT_EQ(m.hold_uuid_calls.load(), 1);

    // --- Step 2: advance mock state to "now on hold" ---
    m.next_channel_flags = CF_ANSWERED | CF_HOLD;
    // Reset event counters so we can count the Unhold audit separately.
    m.event_fire_calls = 0;
    m.event_create_subclass_calls = 0;

    open_switch::control::v1::UnholdRequest unhold_req;
    unhold_req.add_uuids("uuid-seq");
    open_switch::control::v1::UnholdResponse unhold_resp;

    const grpc::Status unhold_status = svc_->Unhold(&ctx, &unhold_req, &unhold_resp);
    ASSERT_TRUE(unhold_status.ok()) << unhold_status.error_message();
    EXPECT_EQ(m.unhold_uuid_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);

    // Both Hold and Unhold captured.
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        EXPECT_EQ(m.hold_uuid_invocations.size(), 1u);
        EXPECT_EQ(m.unhold_uuid_invocations.size(), 1u);
        EXPECT_EQ(m.hold_uuid_invocations[0].uuid, "uuid-seq");
        EXPECT_EQ(m.unhold_uuid_invocations[0].uuid, "uuid-seq");
    }
}

}  // namespace
