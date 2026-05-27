/*
 * tests/unit/control/hangup_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::Hangup against the FS-mock seam.
 *
 * Covered:
 *   - Happy path: session found, channel alive → OK + audit emitted.
 *   - Empty UUID → INVALID_ARGUMENT (no FS session locate call made).
 *   - UUID not found → NOT_FOUND.
 *   - Channel already dead (state >= CS_HANGUP) → FAILED_PRECONDITION.
 *   - Cause code forwarded to switch_channel_hangup.
 *   - Default cause (empty string) maps to NORMAL_CLEARING.
 *   - Audit NOT emitted on failure paths.
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

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0x5E55);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xC4A1);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xEA01);

class HangupHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    // Prime the mock for a successful session locate + alive channel.
    void PrimeAliveSession() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kSession;
        m.next_channel = kChannel;
        // Pre-check sees the channel alive (CS_EXECUTE).
        m.next_channel_get_state = CS_EXECUTE;
        // Audit event.
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, HappyPathReturnsOK) {
    PrimeAliveSession();

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(HangupHandlerTest, HappyPathCallsChannelHangup) {
    PrimeAliveSession();

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    req.set_cause("USER_BUSY");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    svc_->Hangup(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.hangup_invocations.size(), 1u);
    EXPECT_EQ(m.hangup_invocations[0].channel, kChannel);
    EXPECT_EQ(m.hangup_invocations[0].cause, SWITCH_CAUSE_USER_BUSY);
}

TEST_F(HangupHandlerTest, HappyPathAuditEmitted) {
    PrimeAliveSession();

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(HangupHandlerTest, SessionLockedAndUnlocked) {
    PrimeAliveSession();

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    svc_->Hangup(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, EmptyUuidReturnsInvalidArgument) {
    open_switch::control::v1::HangupRequest req;
    req.set_uuid("");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session defaults to nullptr → session not found.
    open_switch::control::v1::HangupRequest req;
    req.set_uuid("nonexistent-uuid");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION (channel already dead)
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, DeadChannelReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    // Pre-check: ChannelGetState returns CS_HANGUP → already dead.
    m.next_channel_get_state = CS_HANGUP;

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    // ChannelHangup NOT called (pre-check fires first).
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    // Audit NOT emitted on failure.
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(HangupHandlerTest, LiveChannelReturnsOKAndEmitsAudit) {
    // Positive-path: channel in CS_EXECUTE → hangup succeeds + audit emitted.
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_channel_get_state = CS_EXECUTE;
    m.next_event = kAuditEvent;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("live-uuid-5678");
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(m.channel_hangup_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// P2-9: variables applied before hangup
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, WithVariablesAppliesThemFirst) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_channel_get_state = CS_EXECUTE;
    m.next_event = kAuditEvent;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    (*req.mutable_variables())["cdr_tag"] = "x";
    (*req.mutable_variables())["agent"] = "y";
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(m.channel_set_variable_calls.load(), 2);
    EXPECT_EQ(m.channel_hangup_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    EXPECT_EQ(m.set_variable_invocations.size(), 2u);
}

TEST_F(HangupHandlerTest, RejectsReservedVarInVariables) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_channel_get_state = CS_EXECUTE;

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    (*req.mutable_variables())["sip_h_Custom"] = "injected";
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Hangup(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    EXPECT_EQ(m.channel_set_variable_calls.load(), 0);
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Default cause (empty → NORMAL_CLEARING)
// ---------------------------------------------------------------------------

TEST_F(HangupHandlerTest, EmptyCauseDefaultsToNormalClearing) {
    PrimeAliveSession();

    open_switch::control::v1::HangupRequest req;
    req.set_uuid("some-uuid-1234");
    // cause field omitted → defaults to ""
    open_switch::control::v1::HangupResponse resp;
    grpc::ServerContext ctx;

    svc_->Hangup(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.hangup_invocations.size(), 1u);
    EXPECT_EQ(m.hangup_invocations[0].cause, SWITCH_CAUSE_NORMAL_CLEARING);
}

}  // namespace
