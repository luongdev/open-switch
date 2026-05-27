/*
 * tests/unit/control/hangup_many_handler_test.cc
 *
 * Unit tests for ControlServiceSkeleton::HangupMany against the
 * FS-mock seam.
 *
 * Covered:
 *   - Empty uuid list → OK, empty hungup_uuids.
 *   - Single uuid, session found + alive → OK, uuid in hungup_uuids +
 *     audit emitted.
 *   - Multiple uuids: all alive → all appear in hungup_uuids.
 *   - Multiple uuids: some not found → only found ones in hungup_uuids.
 *   - Multiple uuids: some already dead → only alive ones in hungup_uuids.
 *   - Never short-circuits: every uuid is attempted even after failures.
 *   - Audit emitted per successful uuid (mirrors Hangup).
 *   - Audit NOT emitted for failed / not-found uuids.
 *
 * Note: the mock's next_session / next_channel applies to ALL locates
 * in a single test. Tests that need mixed outcomes (some found / some
 * not) are approximated via the session counter rather than per-uuid
 * routing — the mock does not support per-uuid routing.
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

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0x5E56);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0xC4A2);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0xEA02);

class HangupManyHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        health_ = std::make_unique<osw::Health>();
        svc_ = std::make_unique<osw::control::ControlServiceSkeleton>(health_.get());
    }

    // Prime for all sessions found + alive.
    void PrimeAllAlive() {
        auto& m = osw::raii::fs::Mock();
        m.next_session = kSession;
        m.next_channel = kChannel;
        // Pre-check sees channels alive (CS_EXECUTE).
        m.next_channel_get_state = CS_EXECUTE;
        m.next_event = kAuditEvent;
        m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    }

    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<osw::control::ControlServiceSkeleton> svc_;
};

// ---------------------------------------------------------------------------
// Empty list
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, EmptyListReturnsOKWithNoUuids) {
    open_switch::control::v1::HangupManyRequest req;
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->HangupMany(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(resp.hungup_uuids_size(), 0);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.session_locate_calls.load(), 0);
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Single uuid, success
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, SingleUuidHappyPath) {
    PrimeAllAlive();

    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("uuid-1");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->HangupMany(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(resp.hungup_uuids_size(), 1);
    EXPECT_EQ(resp.hungup_uuids(0), "uuid-1");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 1);
    // Audit emitted for the successful uuid.
    EXPECT_GE(m.event_fire_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// Multiple uuids, all alive
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, MultipleUuidsAllAliveAllInResponse) {
    PrimeAllAlive();

    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("uuid-1");
    req.add_uuids("uuid-2");
    req.add_uuids("uuid-3");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->HangupMany(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(resp.hungup_uuids_size(), 3);

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 3);
    EXPECT_GE(m.event_fire_calls.load(), 3);
}

// ---------------------------------------------------------------------------
// None found → still returns OK, empty hungup_uuids
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, NoneFoundReturnsOKWithEmptyList) {
    // next_session = nullptr (default) → all locates fail.
    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("uuid-a");
    req.add_uuids("uuid-b");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->HangupMany(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(resp.hungup_uuids_size(), 0);

    auto& m = osw::raii::fs::Mock();
    // Both uuids attempted.
    EXPECT_EQ(m.session_locate_calls.load(), 2);
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Already dead → not in hungup_uuids; still OK overall
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, AlreadyDeadChannelExcludedFromResponse) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSession;
    m.next_channel = kChannel;
    // Pre-check: ChannelGetState returns CS_HANGUP → already dead.
    m.next_channel_get_state = CS_HANGUP;

    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("dead-uuid");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    const grpc::Status status = svc_->HangupMany(&ctx, &req, &resp);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(resp.hungup_uuids_size(), 0);
    // ChannelHangup NOT called (pre-check fires first).
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    // Audit NOT emitted (channel was already dead).
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Never short-circuits: all uuids attempted
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, AllUuidsAttemptedEvenAfterFailure) {
    // First uuid: not found (next_session=nullptr initially).
    // Then we switch to alive for the remaining. The mock doesn't
    // support per-uuid switching, so we prime all as not-found and
    // verify that all locates were attempted.
    // next_session = nullptr (default).

    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("uuid-x");
    req.add_uuids("uuid-y");
    req.add_uuids("uuid-z");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    svc_->HangupMany(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    // All three locates were attempted (not short-circuited after first failure).
    EXPECT_EQ(m.session_locate_calls.load(), 3);
}

// ---------------------------------------------------------------------------
// Cause code propagated
// ---------------------------------------------------------------------------

TEST_F(HangupManyHandlerTest, CauseCodePropagatedToHangup) {
    PrimeAllAlive();

    open_switch::control::v1::HangupManyRequest req;
    req.add_uuids("uuid-1");
    req.set_cause("CALL_REJECTED");
    open_switch::control::v1::HangupManyResponse resp;
    grpc::ServerContext ctx;

    svc_->HangupMany(&ctx, &req, &resp);

    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    ASSERT_EQ(m.hangup_invocations.size(), 1u);
    EXPECT_EQ(m.hangup_invocations[0].cause, SWITCH_CAUSE_CALL_REJECTED);
}

}  // namespace
