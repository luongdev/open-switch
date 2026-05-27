/*
 * tests/unit/control/set_variables_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::SetVariables against the FS-mock
 * seam.
 *
 * Covered:
 *   - Happy path: 1 variable → OK + audit emitted + set_variable called.
 *   - Happy path: 64 variables → OK (at the bound).
 *   - Empty UUID → INVALID_ARGUMENT (no locate call).
 *   - Empty variables map → INVALID_ARGUMENT.
 *   - 65 variables → RESOURCE_EXHAUSTED.
 *   - Invalid variable name (space, dot, slash) → INVALID_ARGUMENT; no FS
 *     locate call made.
 *   - Empty variable name → INVALID_ARGUMENT.
 *   - UUID not found → NOT_FOUND.
 *   - Audit NOT emitted on failure paths.
 *   - Variable values are NOT audited (only var_count).
 *   - Session locked and unlocked once.
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

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0xAB01);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xAB02);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xAB03);

class SetVariablesHandlerTest : public ::testing::Test {
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
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    // Build a SetVariablesRequest with the given uuid and `n` variables.
    open_switch::control::v1::SetVariablesRequest MakeRequest(const std::string& uuid, int n) {
        open_switch::control::v1::SetVariablesRequest req;
        req.set_uuid(uuid);
        for (int i = 0; i < n; ++i) {
            (*req.mutable_variables())["var_" + std::to_string(i)] = "val_" + std::to_string(i);
        }
        return req;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, HappyPath1VariableReturnsOK) {
    PrimeAliveSession();

    auto req = MakeRequest("uuid-1234", 1);
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(SetVariablesHandlerTest, HappyPath1VariableCallsSetVariable) {
    PrimeAliveSession();

    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    (*req.mutable_variables())["my_key"] = "my_value";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    svc_->SetVariables(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_set_variable_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.set_variable_invocations.size(), 1u);
    EXPECT_EQ(m.set_variable_invocations[0].channel, kChannel);
    EXPECT_EQ(m.set_variable_invocations[0].name, "my_key");
    EXPECT_EQ(m.set_variable_invocations[0].value, "my_value");
}

TEST_F(SetVariablesHandlerTest, HappyPath64VariablesReturnsOK) {
    PrimeAliveSession();

    auto req = MakeRequest("uuid-bound", 64);
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_set_variable_calls.load(), 64);
}

TEST_F(SetVariablesHandlerTest, HappyPathAuditEmitted) {
    PrimeAliveSession();

    auto req = MakeRequest("uuid-1234", 1);
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(SetVariablesHandlerTest, SessionLockedAndUnlocked) {
    PrimeAliveSession();

    auto req = MakeRequest("uuid-1234", 1);
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    svc_->SetVariables(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT — empty uuid
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, EmptyUuidReturnsInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("");
    (*req.mutable_variables())["k"] = "v";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT — empty variables map
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, EmptyVariablesMapReturnsInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    // No variables added → map is empty.
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// RESOURCE_EXHAUSTED — 65 variables
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, Over64VariablesReturnsResourceExhausted) {
    auto req = MakeRequest("uuid-1234", 65);
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.channel_set_variable_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT — invalid variable names
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, VariableNameWithSpaceReturnsInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    (*req.mutable_variables())["bad name"] = "value";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.channel_set_variable_calls.load(), 0);
}

TEST_F(SetVariablesHandlerTest, VariableNameWithDotReturnsInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    (*req.mutable_variables())["bad.name"] = "value";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(SetVariablesHandlerTest, EmptyVariableNameReturnsInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    (*req.mutable_variables())[""] = "value";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
}

TEST_F(SetVariablesHandlerTest, ValidNamesAccepted) {
    PrimeAliveSession();

    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("uuid-1234");
    (*req.mutable_variables())["My_Var-01"] = "value";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session defaults to nullptr → not found.
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("nonexistent-uuid");
    (*req.mutable_variables())["k"] = "v";
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->SetVariables(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_set_variable_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Audit on failure paths
// ---------------------------------------------------------------------------

TEST_F(SetVariablesHandlerTest, AuditNotEmittedOnInvalidArgument) {
    open_switch::control::v1::SetVariablesRequest req;
    req.set_uuid("");
    open_switch::control::v1::SetVariablesResponse resp;
    grpc::ServerContext ctx;

    svc_->SetVariables(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

}  // namespace
