/*
 * tests/unit/control/execute_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::Execute against the FS-mock seam.
 *
 * Covered:
 *   - Happy path: valid UUID + allowed app → OK + audit emitted.
 *   - Every allowed app in the V1 allow-list is accepted.
 *   - Disallowed app names → INVALID_ARGUMENT (no FS call made).
 *   - Empty UUID → INVALID_ARGUMENT.
 *   - Empty app → INVALID_ARGUMENT.
 *   - UUID not found → NOT_FOUND.
 *   - Channel dead before execute → FAILED_PRECONDITION.
 *   - FS execute returns non-success → FAILED_PRECONDITION.
 *   - SessionGuard locked and unlocked on success.
 *   - ExecuteApplication called with correct session/app/args.
 *   - Empty args passes nullptr to FS.
 *   - Audit NOT emitted on failure paths.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/observability/health.h"
#include "osw/raii/fs_mock.h"

#include "src/control/control_service_skeleton.h"

namespace {

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0x5E55);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xC4A1);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xEA02);

constexpr const char* kUuid = "test-uuid-1234";

class ExecuteHandlerTest : public ::testing::Test {
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
        m.next_channel_get_state = CS_EXECUTE;
        m.next_execute_application_status = SWITCH_STATUS_SUCCESS;
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    grpc::Status CallExecute(const std::string& uuid,
                             const std::string& app,
                             const std::string& args = "") {
        open_switch::control::v1::ExecuteRequest req;
        req.set_uuid(uuid);
        req.set_app(app);
        req.set_args(args);
        open_switch::control::v1::ExecuteResponse resp;
        grpc::ServerContext ctx;
        return svc_->Execute(&ctx, &req, &resp);
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, HappyPathReturnsOK) {
    PrimeAliveSession();
    const grpc::Status status = CallExecute(kUuid, "playback", "silence_stream://500");
    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(ExecuteHandlerTest, HappyPathCallsExecuteApplication) {
    PrimeAliveSession();
    CallExecute(kUuid, "set", "my_var=hello");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.execute_application_calls.load(), 1);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.execute_application_invocations.size(), 1u);
    EXPECT_EQ(m.execute_application_invocations[0].session, kSession);
    EXPECT_EQ(m.execute_application_invocations[0].app, "set");
    EXPECT_EQ(m.execute_application_invocations[0].args, "my_var=hello");
}

TEST_F(ExecuteHandlerTest, HappyPathAuditEmitted) {
    PrimeAliveSession();
    const grpc::Status status = CallExecute(kUuid, "answer");
    ASSERT_TRUE(status.ok());

    auto& m = osw::raii::fs::Mock();
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(ExecuteHandlerTest, HappyPathSessionLockedAndUnlocked) {
    PrimeAliveSession();
    CallExecute(kUuid, "hangup");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 1);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

TEST_F(ExecuteHandlerTest, EmptyArgsPassesNullptrToFs) {
    PrimeAliveSession();
    CallExecute(kUuid, "answer", "");  // empty args

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.execute_application_invocations.size(), 1u);
    // Mock captures empty string for a nullptr arg.
    EXPECT_EQ(m.execute_application_invocations[0].args, "");
}

// ---------------------------------------------------------------------------
// Allow-list enforcement
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, AllAllowedAppsAreAccepted) {
    const std::vector<std::string> allowed_apps{
        "playback",
        "bridge",
        "transfer",
        "set",
        "hangup",
        "answer",
        "play_and_get_digits",
    };
    for (const auto& app : allowed_apps) {
        osw::raii::fs::MockReset();
        PrimeAliveSession();
        const grpc::Status status = CallExecute(kUuid, app);
        EXPECT_TRUE(status.ok()) << "App '" << app << "' was rejected: " << status.error_message();
    }
}

TEST_F(ExecuteHandlerTest, DisallowedAppReturnsInvalidArgument) {
    const grpc::Status status = CallExecute(kUuid, "system");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.execute_application_calls.load(), 0);
}

TEST_F(ExecuteHandlerTest, DisallowedAppsIncludeCommonDangerousOnes) {
    const std::vector<std::string> denied{
        "system",
        "shell",
        "lua",
        "javascript",
        "python",
        "exec",
        "socket",
        "info",
        "log",
    };
    for (const auto& app : denied) {
        osw::raii::fs::MockReset();
        const grpc::Status status = CallExecute(kUuid, app);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT)
            << "App '" << app << "' should be denied but was not";
        auto& m = osw::raii::fs::Mock();
        EXPECT_EQ(m.execute_application_calls.load(), 0)
            << "App '" << app << "' reached FS despite being denied";
    }
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, EmptyUuidReturnsInvalidArgument) {
    const grpc::Status status = CallExecute("", "playback");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
}

TEST_F(ExecuteHandlerTest, EmptyAppReturnsInvalidArgument) {
    const grpc::Status status = CallExecute(kUuid, "");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, UnknownUuidReturnsNotFound) {
    // next_session defaults to nullptr.
    const grpc::Status status = CallExecute(kUuid, "answer");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.execute_application_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, DeadChannelReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_channel_get_state = CS_HANGUP;  // already dead

    const grpc::Status status = CallExecute(kUuid, "playback", "silence://500");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.execute_application_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(ExecuteHandlerTest, FsExecuteFailureReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    m.next_channel_get_state = CS_EXECUTE;
    m.next_execute_application_status = SWITCH_STATUS_GENERR;

    const grpc::Status status = CallExecute(kUuid, "answer");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    EXPECT_EQ(m.execute_application_calls.load(), 1);
    EXPECT_EQ(m.event_fire_calls.load(), 0);  // audit NOT emitted
}

// ---------------------------------------------------------------------------
// Audit NOT emitted on failures
// ---------------------------------------------------------------------------

TEST_F(ExecuteHandlerTest, AuditNotEmittedOnNotFound) {
    const grpc::Status status = CallExecute(kUuid, "answer");
    EXPECT_NE(status.error_code(), grpc::StatusCode::OK);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

}  // namespace
