/*
 * tests/unit/control/blind_transfer_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::BlindTransfer against the
 * FS-mock seam.
 *
 * Covered:
 *   - Happy path: UUID found, destination set → OK + audit emitted.
 *   - Happy path with explicit dialplan + context forwarded verbatim.
 *   - Empty UUID → INVALID_ARGUMENT.
 *   - Empty destination → INVALID_ARGUMENT.
 *   - UUID not found → NOT_FOUND.
 *   - FS returns non-success → FAILED_PRECONDITION.
 *   - SessionGuard locked and unlocked.
 *   - Optional NULL pass-through: empty dialplan/context → nullptr in
 *     FS capture (FF-025).
 *   - Non-empty dialplan/context → passed verbatim.
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

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0xBEEF);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xC0DE);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xEA03);

constexpr const char* kUuid = "btransfer-uuid-5678";
constexpr const char* kDest = "1001";

class BlindTransferHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    void PrimeAliveSession() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kSession;
        m.next_channel = kChannel;
        m.next_session_transfer_status = SWITCH_STATUS_SUCCESS;
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    grpc::Status CallBlindTransfer(const std::string& uuid,
                                   const std::string& dest,
                                   const std::string& dialplan = "",
                                   const std::string& context = "") {
        open_switch::control::v1::BlindTransferRequest req;
        req.set_uuid(uuid);
        req.set_destination(dest);
        req.set_dialplan(dialplan);
        req.set_context(context);
        open_switch::control::v1::BlindTransferResponse resp;
        grpc::ServerContext ctx;
        return svc_->BlindTransfer(&ctx, &req, &resp);
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, HappyPathReturnsOK) {
    PrimeAliveSession();
    const grpc::Status status = CallBlindTransfer(kUuid, kDest);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(BlindTransferHandlerTest, HappyPathCallsSessionTransfer) {
    PrimeAliveSession();
    CallBlindTransfer(kUuid, kDest);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_transfer_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.session_transfer_invocations.size(), 1u);
    EXPECT_EQ(m.session_transfer_invocations[0].session, kSession);
    EXPECT_EQ(m.session_transfer_invocations[0].extension, kDest);
}

TEST_F(BlindTransferHandlerTest, HappyPathAuditEmitted) {
    PrimeAliveSession();
    const grpc::Status status = CallBlindTransfer(kUuid, kDest);
    ASSERT_TRUE(status.ok());

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(BlindTransferHandlerTest, HappyPathSessionLockedAndUnlocked) {
    PrimeAliveSession();
    CallBlindTransfer(kUuid, kDest);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// Optional-NULL pass-through (FF-025)
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, EmptyDialplanPassesNullToFs) {
    PrimeAliveSession();
    // dialplan not set → empty string → nullptr to FS.
    CallBlindTransfer(kUuid, kDest, /*dialplan=*/"", /*context=*/"");

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.session_transfer_invocations.size(), 1u);
    EXPECT_TRUE(m.session_transfer_invocations[0].dialplan_was_null);
    EXPECT_TRUE(m.session_transfer_invocations[0].context_was_null);
}

TEST_F(BlindTransferHandlerTest, NonEmptyDialplanPassedVerbatim) {
    PrimeAliveSession();
    CallBlindTransfer(kUuid, kDest, /*dialplan=*/"XML", /*context=*/"default");

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.session_transfer_invocations.size(), 1u);
    EXPECT_FALSE(m.session_transfer_invocations[0].dialplan_was_null);
    EXPECT_FALSE(m.session_transfer_invocations[0].context_was_null);
    EXPECT_EQ(m.session_transfer_invocations[0].dialplan, "XML");
    EXPECT_EQ(m.session_transfer_invocations[0].context, "default");
}

TEST_F(BlindTransferHandlerTest, OnlyDialplanSetContextIsNull) {
    PrimeAliveSession();
    CallBlindTransfer(kUuid, kDest, /*dialplan=*/"XML", /*context=*/"");

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.session_transfer_invocations.size(), 1u);
    EXPECT_FALSE(m.session_transfer_invocations[0].dialplan_was_null);
    EXPECT_TRUE(m.session_transfer_invocations[0].context_was_null);
    EXPECT_EQ(m.session_transfer_invocations[0].dialplan, "XML");
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, EmptyUuidReturnsInvalidArgument) {
    const grpc::Status status = CallBlindTransfer("", kDest);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.session_transfer_calls.load(), 0);
}

TEST_F(BlindTransferHandlerTest, EmptyDestinationReturnsInvalidArgument) {
    const grpc::Status status = CallBlindTransfer(kUuid, "");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.session_transfer_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session defaults to nullptr.
    const grpc::Status status = CallBlindTransfer(kUuid, kDest);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_transfer_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION (FS failure)
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, FsTransferFailureReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_session_transfer_status = SWITCH_STATUS_GENERR;

    const grpc::Status status = CallBlindTransfer(kUuid, kDest);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.session_transfer_calls.load(), 1);
    EXPECT_EQ(m.event_fire_calls.load(), 0);  // audit NOT emitted on failure
}

// ---------------------------------------------------------------------------
// Audit NOT emitted on failures
// ---------------------------------------------------------------------------

TEST_F(BlindTransferHandlerTest, AuditNotEmittedOnNotFound) {
    const grpc::Status status = CallBlindTransfer(kUuid, kDest);
    EXPECT_NE(status.error_code(), grpc::StatusCode::OK);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

}  // namespace
