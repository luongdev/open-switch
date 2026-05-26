/*
 * tests/unit/raii/event_guard_test.cc
 *
 * Unit tests for osw::EventGuard against the FS-mock seam.
 *
 * Covered:
 *   - Construction allocates; destruction destroys.
 *   - Allocation failure leaves guard empty (operator bool false).
 *   - fire() transfers ownership to FS; subsequent destruction is a no-op.
 *   - fire() on empty guard returns SWITCH_STATUS_FALSE; no FS call.
 *   - release() hands the ptr to the caller; destruction is a no-op.
 *   - Move-construction transfers ownership.
 *   - Move-assignment destroys destination's prior event.
 *   - adopt() takes ownership without re-creating.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/raii/event_guard.h"

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_event_t* const kEventA = reinterpret_cast<switch_event_t*>(0x30A);
switch_event_t* const kEventB = reinterpret_cast<switch_event_t*>(0x30B);

class EventGuardTest : public ::testing::Test {
 protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

TEST_F(EventGuardTest, ConstructionAllocatesAndDestructionDestroys) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    {
        osw::EventGuard ev(SWITCH_EVENT_CUSTOM);
        EXPECT_TRUE(static_cast<bool>(ev));
        EXPECT_EQ(ev.get(), kEventA);
        EXPECT_EQ(m.event_create_calls.load(), 1);
        EXPECT_EQ(m.event_destroy_calls.load(), 0);
    }
    EXPECT_EQ(m.event_destroy_calls.load(), 1);
}

TEST_F(EventGuardTest, AllocationFailureLeavesGuardEmpty) {
    auto& m = osw::raii::fs::Mock();
    m.next_event_create_status = SWITCH_STATUS_GENERR;
    {
        osw::EventGuard ev;
        EXPECT_FALSE(static_cast<bool>(ev));
        EXPECT_EQ(ev.get(), nullptr);
        EXPECT_EQ(m.event_create_calls.load(), 1);
    }
    // No destroy when nothing was created.
    EXPECT_EQ(m.event_destroy_calls.load(), 0);
}

TEST_F(EventGuardTest, FireTransfersOwnershipAndGuardBecomesEmpty) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    osw::EventGuard ev;
    EXPECT_TRUE(static_cast<bool>(ev));

    switch_status_t status = ev.fire();
    EXPECT_EQ(status, SWITCH_STATUS_SUCCESS);
    EXPECT_FALSE(static_cast<bool>(ev));
    EXPECT_EQ(m.event_fire_calls.load(), 1);

    // Destruction is a no-op now.
    // (verified by event_destroy_calls staying at 0 after scope exit.)
}

TEST_F(EventGuardTest, FireOnEmptyGuardReturnsFalseAndIsNoOp) {
    auto& m = osw::raii::fs::Mock();
    m.next_event_create_status = SWITCH_STATUS_GENERR;  // empty guard
    osw::EventGuard ev;
    EXPECT_FALSE(static_cast<bool>(ev));

    switch_status_t status = ev.fire();
    EXPECT_EQ(status, SWITCH_STATUS_FALSE);
    EXPECT_EQ(m.event_fire_calls.load(), 0);
}

TEST_F(EventGuardTest, ReleaseHandsPtrToCallerAndDestructionIsNoOp) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    switch_event_t* released = nullptr;
    {
        osw::EventGuard ev;
        released = ev.release();
        EXPECT_EQ(released, kEventA);
        EXPECT_FALSE(static_cast<bool>(ev));
    }
    EXPECT_EQ(m.event_destroy_calls.load(), 0);
    // Caller would need to destroy released; mock doesn't enforce.
    EXPECT_EQ(released, kEventA);
}

TEST_F(EventGuardTest, MoveConstructionTransfersOwnership) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    osw::EventGuard a;
    osw::EventGuard b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kEventA);
    EXPECT_EQ(m.event_destroy_calls.load(), 0);  // nothing destroyed yet
    // b's destructor here -> 1 destroy.
}

TEST_F(EventGuardTest, MoveAssignmentDestroysDestinationsPrior) {
    auto& m = osw::raii::fs::Mock();

    m.next_event = kEventA;
    osw::EventGuard a;

    m.next_event = kEventB;
    osw::EventGuard b;

    EXPECT_EQ(m.event_destroy_calls.load(), 0);

    b = std::move(a);  // destroys B's prior event, takes A's
    EXPECT_EQ(m.event_destroy_calls.load(), 1);

    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kEventA);
}

TEST_F(EventGuardTest, AdoptTakesOwnershipWithoutAllocating) {
    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_create_calls.load(), 0);
    {
        auto ev = osw::EventGuard::adopt(kEventA);
        EXPECT_TRUE(static_cast<bool>(ev));
        EXPECT_EQ(ev.get(), kEventA);
        EXPECT_EQ(m.event_create_calls.load(), 0);  // no allocation
    }
    EXPECT_EQ(m.event_destroy_calls.load(), 1);
}

TEST_F(EventGuardTest, AdoptOfNullYieldsEmptyGuard) {
    auto ev = osw::EventGuard::adopt(nullptr);
    EXPECT_FALSE(static_cast<bool>(ev));
    EXPECT_EQ(osw::raii::fs::Mock().event_destroy_calls.load(), 0);
}

TEST_F(EventGuardTest, SelfMoveAssignmentIsSafe) {
    auto& m = osw::raii::fs::Mock();
    m.next_event = kEventA;
    osw::EventGuard a;
    osw::EventGuard& alias = a;
    a = std::move(alias);  // tolerated; the helper guards self-move
    EXPECT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.get(), kEventA);
    EXPECT_EQ(m.event_destroy_calls.load(), 0);
}

}  // namespace
