/*
 * tests/unit/raii/session_lock_test.cc
 *
 * Unit tests for osw::SessionLock against the FS-mock seam.
 * See tests/unit/raii/README.md for why we use the macro-based mock
 * seam (header-only, OSW_TEST_FS_MOCK=1) rather than gmock or DI.
 *
 * Covered:
 *   - Empty construction (null UUID, missing UUID).
 *   - Construction-locks / destruction-unlocks pairing.
 *   - operator bool() reflects holds-or-not.
 *   - reset() drops the lock eagerly and is idempotent.
 *   - Move-construction transfers ownership.
 *   - Move-assignment releases the destination's prior lock.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/raii/session_lock.h"

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

// Sentinel pointers used as opaque "session"/"channel" addresses. The
// mock never dereferences them.
switch_core_session_t* const kSessionA = reinterpret_cast<switch_core_session_t*>(0x10A);
switch_core_session_t* const kSessionB = reinterpret_cast<switch_core_session_t*>(0x10B);
switch_channel_t* const kChannelA = reinterpret_cast<switch_channel_t*>(0x20A);

class SessionLockTest : public ::testing::Test {
  protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

TEST_F(SessionLockTest, NullUuidProducesEmptyLock) {
    osw::SessionLock lock(nullptr);
    EXPECT_FALSE(static_cast<bool>(lock));
    EXPECT_EQ(lock.get(), nullptr);
    EXPECT_EQ(lock.channel(), nullptr);
    EXPECT_EQ(osw::raii::fs::Mock().session_locate_calls.load(), 0);
}

TEST_F(SessionLockTest, MissingUuidYieldsEmptyLockButCallsLocate) {
    // mock returns null session (default).
    osw::SessionLock lock("uuid-missing");
    EXPECT_FALSE(static_cast<bool>(lock));
    EXPECT_EQ(osw::raii::fs::Mock().session_locate_calls.load(), 1);
    EXPECT_EQ(osw::raii::fs::Mock().session_rwunlock_calls.load(), 0);
    // Destruction of empty lock: no extra rwunlock.
}

TEST_F(SessionLockTest, FoundUuidLocksAndUnlocksOnDestruction) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSessionA;
    m.next_channel = kChannelA;
    {
        osw::SessionLock lock("uuid-found");
        EXPECT_TRUE(static_cast<bool>(lock));
        EXPECT_EQ(lock.get(), kSessionA);
        EXPECT_EQ(lock.channel(), kChannelA);
        EXPECT_EQ(m.session_locate_calls.load(), 1);
        EXPECT_EQ(m.session_rwunlock_calls.load(), 0);
    }
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}

TEST_F(SessionLockTest, ResetDropsLockEagerlyAndIsIdempotent) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSessionA;
    osw::SessionLock lock("uuid-found");
    EXPECT_TRUE(static_cast<bool>(lock));
    lock.reset();
    EXPECT_FALSE(static_cast<bool>(lock));
    EXPECT_EQ(lock.get(), nullptr);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);

    lock.reset();  // second call must be a no-op
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
    // Destruction of already-reset lock: still no extra rwunlock.
}

TEST_F(SessionLockTest, MoveConstructionTransfersOwnership) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSessionA;
    osw::SessionLock a("uuid");
    osw::SessionLock b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));  // NOLINT(*-use-after-move)
    EXPECT_EQ(a.get(), nullptr);         // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kSessionA);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 0);  // not yet
    // b's destructor here.
}

TEST_F(SessionLockTest, MoveAssignmentReleasesDestinationPrior) {
    auto& m = osw::raii::fs::Mock();

    m.next_session = kSessionA;
    osw::SessionLock a("uuid-a");

    m.next_session = kSessionB;
    osw::SessionLock b("uuid-b");

    EXPECT_EQ(m.session_rwunlock_calls.load(), 0);
    b = std::move(a);  // releases B's lock, takes A's
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);

    EXPECT_FALSE(static_cast<bool>(a));  // NOLINT(*-use-after-move)
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.get(), kSessionA);

    // b's destructor unlocks A.
}

TEST_F(SessionLockTest, SelfMoveAssignmentIsSafe) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = kSessionA;
    osw::SessionLock a("uuid");
    osw::SessionLock& alias = a;
    a = std::move(alias);  // tolerated; the helper guards self-move
    EXPECT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.get(), kSessionA);
    EXPECT_EQ(m.session_rwunlock_calls.load(), 0);
}

}  // namespace
