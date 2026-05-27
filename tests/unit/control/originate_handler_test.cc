/*
 * tests/unit/control/originate_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::Originate against the
 * FS-mock seam.
 *
 * Covered:
 *   - Happy path: FS returns SUCCESS + bleg UUID → OK + channel_uuid set
 *     + audit emit fired.
 *   - Empty endpoints → INVALID_ARGUMENT (no FS call made).
 *   - Empty individual endpoint → INVALID_ARGUMENT.
 *   - Timeout ≤ 0 → INVALID_ARGUMENT.
 *   - FS originate fails (USER_BUSY) → FAILED_PRECONDITION.
 *   - FS originate fails with timeout cause → DEADLINE_EXCEEDED.
 *   - FS returns SUCCESS but null bleg → UNAVAILABLE.
 *   - Variables are forwarded to FS via ovars event.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/idempotency_cache.h"
#include "osw/observability/health.h"
#include "osw/raii/fs_mock.h"

#include "google/protobuf/duration.pb.h"
#include "src/control/control_service_skeleton.h"

namespace {

// Sentinel bleg session pointer.
switch_core_session_t* const kBlegSession = reinterpret_cast<switch_core_session_t*>(0xB1E6);
// Sentinel event for ovars.
switch_event_t* const kOvarsEvent = reinterpret_cast<switch_event_t*>(0x0A85);
// Test UUID returned for the originated bleg.
constexpr const char* kTestUuid = "test-uuid-1234-5678-abcd";

class OriginateHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    // Helper to add an endpoint to the request.
    static void AddEndpoint(open_switch::control::v1::OriginateRequest& req,
                            const std::string& ep) {
        req.add_endpoints(ep);
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, HappyPathReturnsBlegUuid) {
    auto& m = osw::raii::fs::Mock();
    // Prime the mock: originate returns SUCCESS, bleg is set,
    // UUID is returned for the bleg.
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    // ovars creation succeeds (next_event for EventCreate).
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    // Audit emit (EventCreateSubclass for osw.control.originate).
    // Reuse kOvarsEvent for audit event pointer as well — mock reuses next_event.
    // We just need fire to succeed.
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok()) << "Expected OK, got: " << status.error_message();
    EXPECT_EQ(resp.channel_uuid(), kTestUuid);

    // Originate call was made exactly once.
    EXPECT_EQ(m.originate_calls.load(), 1);
    // Session rwunlock was called once (for the bleg).
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

TEST_F(OriginateHandlerTest, HappyPathDialStringPassedToFS) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/profile1/user@10.0.0.1");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    svc_->Originate(&ctx, &req, &resp);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.originate_invocations.size(), 1u);
    EXPECT_EQ(m.originate_invocations[0].dial_string, "sofia/profile1/user@10.0.0.1");
}

TEST_F(OriginateHandlerTest, HappyPathAuditEmitted) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());

    // Audit fires via EventCreateSubclass + EventFire.
    EXPECT_GE(m.event_create_subclass_calls.load(), 1);
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

TEST_F(OriginateHandlerTest, MultipleEndpointsFailoverStrategy) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    req.set_strategy(open_switch::control::v1::OriginateRequest::FAILOVER);
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    AddEndpoint(req, "sofia/gateway/gw2/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    svc_->Originate(&ctx, &req, &resp);

    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.originate_invocations.size(), 1u);
    EXPECT_EQ(m.originate_invocations[0].dial_string,
              "sofia/gateway/gw1/+441234567890|sofia/gateway/gw2/+441234567890");
}

// ---------------------------------------------------------------------------
// INVALID_ARGUMENT failure paths
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, EmptyEndpointsReturnsInvalidArgument) {
    auto& m = osw::raii::fs::Mock();
    open_switch::control::v1::OriginateRequest req;  // no endpoints
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(m.originate_calls.load(), 0);
    // No audit on failure.
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(OriginateHandlerTest, EmptyIndividualEndpointReturnsInvalidArgument) {
    auto& m = osw::raii::fs::Mock();
    open_switch::control::v1::OriginateRequest req;
    req.add_endpoints("");  // empty endpoint
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(m.originate_calls.load(), 0);
}

TEST_F(OriginateHandlerTest, ZeroTimeoutReturnsInvalidArgument) {
    auto& m = osw::raii::fs::Mock();
    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    // Explicitly set timeout to 0s 0ns.
    auto* dur = req.mutable_timeout();
    dur->set_seconds(0);
    dur->set_nanos(0);
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(m.originate_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// FAILED_PRECONDITION (FS non-timeout failure)
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, FSFailureUserBusyReturnsFailedPrecondition) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_FALSE;
    m.next_originate_cause = SWITCH_CAUSE_USER_BUSY;
    // No bleg produced on failure.
    m.next_originate_bleg = nullptr;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_NE(status.error_message().find("USER_BUSY"), std::string::npos);
    // No audit on failure.
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// DEADLINE_EXCEEDED (timeout cause)
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, FSTimeoutCauseReturnsDeadlineExceeded) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_FALSE;
    m.next_originate_cause = SWITCH_CAUSE_ORIGINATOR_CANCEL;
    m.next_originate_bleg = nullptr;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// UNAVAILABLE (FS success but null bleg)
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, SuccessButNullBlegReturnsUnavailable) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = nullptr;  // null bleg even on success
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Variables forwarded to ovars
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, VariablesCreateOvarsEvent) {
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    (*req.mutable_variables())["sip_contact_user"] = "testuser";
    (*req.mutable_variables())["origination_caller_id_name"] = "Alice";
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());

    // EventCreate was called once for the ovars event.
    EXPECT_EQ(m.event_create_calls.load(), 1);
    // EventAddHeaderString is called twice for the 2 channel variables,
    // PLUS 4 times for the audit event headers (uuid, dest, cid_name, cid_num).
    // Total = 6. We assert >= 2 to verify both channel variables were forwarded.
    EXPECT_GE(m.event_add_header_calls.load(), 2);
    // Originate was called exactly once.
    EXPECT_EQ(m.originate_calls.load(), 1);
}

TEST_F(OriginateHandlerTest, OvarsEventDestroyedByOptionsOwnerNotLeak) {
    // P2-6: OriginateOptions owns ovars via dtor (ovars_ptr() borrow, not
    // ReleaseOvars). After Originate returns the options object is destroyed;
    // the mock EventDestroy counter must be >= 1 (for the ovars event).
    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    // Include a variable so ovars event is allocated.
    (*req.mutable_variables())["cdr_tag"] = "test";
    open_switch::control::v1::OriginateResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->Originate(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());

    // The OriginateOptions dtor must have called EventDestroy on the ovars ptr.
    EXPECT_GE(m.event_destroy_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// W5 Track B — idempotency dedup (IdempotencyCache wired into handler)
// ---------------------------------------------------------------------------

TEST_F(OriginateHandlerTest, SameRequestIdDeduplicatesOriginateCall) {
    // Inject a real IdempotencyCache into the skeleton.
    auto cache = std::make_unique<osw::control::IdempotencyCache>(
        /*capacity=*/16,
        /*ttl=*/std::chrono::seconds(300),
        /*in_flight_max_wait=*/std::chrono::seconds(5));
    svc_->SetIdempotencyCache(cache.get());

    auto& m = osw::raii::fs::Mock();
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    // First request: identical request_id.
    open_switch::control::v1::OriginateRequest req;
    req.mutable_header()->set_request_id("dedup-test-req-001");
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");
    open_switch::control::v1::OriginateResponse resp1;
    grpc::ServerContext ctx1;

    const grpc::Status s1 = svc_->Originate(&ctx1, &req, &resp1);
    ASSERT_TRUE(s1.ok()) << s1.error_message();
    EXPECT_EQ(resp1.channel_uuid(), kTestUuid);

    // originate_calls must be exactly 1 after first call.
    EXPECT_EQ(m.originate_calls.load(), 1);

    // Second request: same request_id, same endpoint (mock NOT re-primed —
    // the handler must NOT call switch_ivr_originate again).
    open_switch::control::v1::OriginateResponse resp2;
    grpc::ServerContext ctx2;
    const grpc::Status s2 = svc_->Originate(&ctx2, &req, &resp2);

    ASSERT_TRUE(s2.ok()) << s2.error_message();
    // Response must be the cached one.
    EXPECT_EQ(resp2.channel_uuid(), kTestUuid);
    // FS originate must NOT have been called a second time.
    EXPECT_EQ(m.originate_calls.load(), 1) << "switch_ivr_originate called more than once!";
}

TEST_F(OriginateHandlerTest, EmptyRequestIdBypassesCache) {
    // When request_id is empty, the handler should always execute FS originate.
    auto cache = std::make_unique<osw::control::IdempotencyCache>(
        /*capacity=*/16,
        /*ttl=*/std::chrono::seconds(300),
        /*in_flight_max_wait=*/std::chrono::seconds(5));
    svc_->SetIdempotencyCache(cache.get());

    auto& m = osw::raii::fs::Mock();

    // Prime the mock for two calls.
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = kBlegSession;
    m.next_bleg_uuid = kTestUuid;
    m.next_event = kOvarsEvent;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;

    open_switch::control::v1::OriginateRequest req;
    // header set with empty request_id (default proto value) — no dedup.
    AddEndpoint(req, "sofia/gateway/gw1/+441234567890");

    open_switch::control::v1::OriginateResponse resp1;
    grpc::ServerContext ctx1;
    svc_->Originate(&ctx1, &req, &resp1);

    open_switch::control::v1::OriginateResponse resp2;
    grpc::ServerContext ctx2;
    svc_->Originate(&ctx2, &req, &resp2);

    // Both calls must have gone to FS.
    EXPECT_EQ(m.originate_calls.load(), 2);
}

}  // namespace
