/*
 * tests/unit/raii/media_bug_lease_test.cc
 *
 * Unit tests for osw::MediaBugLease against the FS-mock seam.
 *
 * The lease is ATTACH + REMOVE; W1 ships no production callers (media
 * plane lands in W4). Tests cover the pairing semantics so that when
 * the W4 code starts using it, the contract is already verified.
 *
 * Covered:
 *   - Successful add: bug ptr stored; remove called on destruction.
 *   - Failed add: lease holds null; no remove on destruction.
 *   - Null session: no add call; lease holds null.
 *   - remove() is idempotent.
 *   - Move-construction transfers ownership.
 *   - Move-assignment removes the destination's prior bug.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/raii/media_bug_lease.h"

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_core_session_t* const kSession =
    reinterpret_cast<switch_core_session_t*>(0x40A);
switch_media_bug_t* const kBugA = reinterpret_cast<switch_media_bug_t*>(0x50A);
switch_media_bug_t* const kBugB = reinterpret_cast<switch_media_bug_t*>(0x50B);

// A bug-callback is required by the API signature but we never invoke
// it in mock tests.
switch_bool_t TestCallback(switch_media_bug_t*, void*, switch_abc_type_t) {
    return SWITCH_TRUE;
}

class MediaBugLeaseTest : public ::testing::Test {
 protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

TEST_F(MediaBugLeaseTest, SuccessfulAddStoresBugAndRemovesOnDestruction) {
    auto& m = osw::raii::fs::Mock();
    m.next_bug = kBugA;
    {
        osw::MediaBugLease lease(kSession, "name", "fn", &TestCallback,
                                 nullptr, 0, 0);
        EXPECT_TRUE(static_cast<bool>(lease));
        EXPECT_EQ(lease.get(), kBugA);
        EXPECT_EQ(m.media_bug_add_calls.load(), 1);
        EXPECT_EQ(m.media_bug_remove_calls.load(), 0);
    }
    EXPECT_EQ(m.media_bug_remove_calls.load(), 1);
}

TEST_F(MediaBugLeaseTest, FailedAddLeavesLeaseEmpty) {
    auto& m = osw::raii::fs::Mock();
    m.next_bug_add_status = SWITCH_STATUS_GENERR;
    {
        osw::MediaBugLease lease(kSession, "name", "fn", &TestCallback,
                                 nullptr, 0, 0);
        EXPECT_FALSE(static_cast<bool>(lease));
        EXPECT_EQ(lease.get(), nullptr);
        EXPECT_EQ(m.media_bug_add_calls.load(), 1);
    }
    EXPECT_EQ(m.media_bug_remove_calls.load(), 0);  // nothing to remove
}

TEST_F(MediaBugLeaseTest, NullSessionSkipsAddCall) {
    auto& m = osw::raii::fs::Mock();
    {
        osw::MediaBugLease lease(nullptr, "name", "fn", &TestCallback,
                                 nullptr, 0, 0);
        EXPECT_FALSE(static_cast<bool>(lease));
    }
    EXPECT_EQ(m.media_bug_add_calls.load(), 0);
    EXPECT_EQ(m.media_bug_remove_calls.load(), 0);
}

TEST_F(MediaBugLeaseTest, ExplicitRemoveIsEagerAndIdempotent) {
    auto& m = osw::raii::fs::Mock();
    m.next_bug = kBugA;
    osw::MediaBugLease lease(kSession, "name", "fn", &TestCallback,
                             nullptr, 0, 0);
    EXPECT_TRUE(static_cast<bool>(lease));
    lease.remove();
    EXPECT_FALSE(static_cast<bool>(lease));
    EXPECT_EQ(m.media_bug_remove_calls.load(), 1);

    lease.remove();  // idempotent
    EXPECT_EQ(m.media_bug_remove_calls.load(), 1);
}

TEST_F(MediaBugLeaseTest, MoveConstructionTransfersOwnership) {
    auto& m = osw::raii::fs::Mock();
    m.next_bug = kBugA;
    osw::MediaBugLease a(kSession, "n", "fn", &TestCallback, nullptr, 0, 0);
    osw::MediaBugLease b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kBugA);
    EXPECT_EQ(m.media_bug_remove_calls.load(), 0);
    // b's destructor here -> 1 remove.
}

TEST_F(MediaBugLeaseTest, MoveAssignmentRemovesDestinationsPrior) {
    auto& m = osw::raii::fs::Mock();
    m.next_bug = kBugA;
    osw::MediaBugLease a(kSession, "a", "afn", &TestCallback, nullptr, 0, 0);

    m.next_bug = kBugB;
    osw::MediaBugLease b(kSession, "b", "bfn", &TestCallback, nullptr, 0, 0);

    EXPECT_EQ(m.media_bug_remove_calls.load(), 0);
    b = std::move(a);
    EXPECT_EQ(m.media_bug_remove_calls.load(), 1);
    EXPECT_FALSE(static_cast<bool>(a));   // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kBugA);
    // b's destructor here -> 1 more remove (total 2).
}

}  // namespace
