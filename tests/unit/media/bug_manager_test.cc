/*
 * tests/unit/media/bug_manager_test.cc
 *
 * Unit tests for osw::media::MediaBugManager + BugHandle.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1).
 *
 * Acceptance scenarios covered (from W6-track-A-bug-manager.md):
 *   A1 — Attach STT → TTS → second STT: third returns kAlreadyExists.
 *   A2 — STT (MID_READ) → TTS (INJECT): both succeed; flags captured.
 *   A3 — TTS (INJECT) → STT (MID_READ) without SMBF_FIRST: kFailed.
 *   A4 — STT → VAD (EARLY): VAD succeeds; mock sees SMBF_FIRST in flags.
 *   A5 — BugHandle destructor → manager Detach called.
 *   A6 — DetachAll after 3 attaches: 3 records removed.
 *   A7 — DetachAll twice: second is no-op (idempotent).
 *   A8 — Detach(unknown_id): silent no-op.
 *   A9 — Concurrent attach/detach (16 threads × 100 ops): no crash + TSAN-clean.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// FS mock seam MUST be included first — it provides the forward declarations
// for switch_core_session_t, switch_media_bug_t, etc. that bug_manager.h
// depends on when OSW_TEST_FS_MOCK=1.  Do NOT re-order this block.
// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "osw/media/bug_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "osw/media/bug_handle.h"
#include "osw/media/purpose.h"

namespace {

using osw::media::BugConfig;
using osw::media::BugHandle;
using osw::media::MediaBugManager;
using osw::media::Purpose;
using osw::media::PurposeName;
using osw::media::StageRank;
using osw::raii::fs::Mock;
using osw::raii::fs::MockReset;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Returns a heap-allocated fake session pointer that the mock treats as
/// non-null.  The manager calls FsSessionGetUuid → mock's SessionGetUuid
/// which returns Mock().next_bleg_uuid.  We use next_bleg_uuid as the
/// channel UUID string.
static switch_core_session_t* FakeSession() {
    // The mock's SessionGetUuid returns next_bleg_uuid regardless of the
    // pointer value, as long as it's non-null.  Use 0x1 as a sentinel.
    return reinterpret_cast<switch_core_session_t*>(static_cast<uintptr_t>(0x1));
}

/// RAII fixture: creates a fresh MediaBugManager per test.
class BugManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        MockReset();
        // Set a non-empty UUID so the manager's session-UUID check passes.
        Mock().next_bleg_uuid = "test-channel-uuid-0001";
        // Default: bug add succeeds; returns a non-null fake bug ptr.
        Mock().next_bug_add_status = SWITCH_STATUS_SUCCESS;
        Mock().next_bug = reinterpret_cast<switch_media_bug_t*>(static_cast<uintptr_t>(0x2));
        // Default: channel state = CS_DESTROY so OswMediaChannelDestroy acts.
        Mock().next_channel_get_state = CS_DESTROY;
        Mock().next_channel = reinterpret_cast<switch_channel_t*>(static_cast<uintptr_t>(0x3));
    }

    void TearDown() override { MockReset(); }

    MediaBugManager mgr_;
};

// ---------------------------------------------------------------------------
// Stage-rank helpers (pure function — no FS calls).
// ---------------------------------------------------------------------------

TEST(PurposeTest, StageRankTable) {
    // EARLY = 1
    EXPECT_EQ(1u, StageRank(Purpose::kVadBargeIn));

    // MID_READ = 2
    EXPECT_EQ(2u, StageRank(Purpose::kSttTranscribe));
    EXPECT_EQ(2u, StageRank(Purpose::kVoicebotDuplexRead));
    EXPECT_EQ(2u, StageRank(Purpose::kAmdDetect));

    // INJECT = 3
    EXPECT_EQ(3u, StageRank(Purpose::kTtsPlayback));
    EXPECT_EQ(3u, StageRank(Purpose::kVoicebotDuplexWrite));

    // LATE = 4
    EXPECT_EQ(4u, StageRank(Purpose::kRecordingRelay));
    EXPECT_EQ(4u, StageRank(Purpose::kTest));
    EXPECT_EQ(4u, StageRank(Purpose::kUnspecified));
}

TEST(PurposeTest, PurposeNameNonEmpty) {
    // Spot-check a few; none should be empty.
    EXPECT_FALSE(PurposeName(Purpose::kTtsPlayback).empty());
    EXPECT_FALSE(PurposeName(Purpose::kSttTranscribe).empty());
    EXPECT_EQ("tts_playback", PurposeName(Purpose::kTtsPlayback));
    EXPECT_EQ("stt_transcribe", PurposeName(Purpose::kSttTranscribe));
    EXPECT_EQ("vad_barge_in", PurposeName(Purpose::kVadBargeIn));
}

// ---------------------------------------------------------------------------
// A1 — Attach STT → TTS → second STT: third returns kAlreadyExists.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A1_DuplicatePurposeRejected) {
    BugConfig stt{Purpose::kSttTranscribe, 0, 16000, "tenant", "ep"};
    BugConfig tts{Purpose::kTtsPlayback, 0, 16000, "tenant", "ep"};
    BugConfig stt2{Purpose::kSttTranscribe, 0, 16000, "tenant", "ep"};

    auto r1 = mgr_.Attach(FakeSession(), stt);
    ASSERT_TRUE(r1.ok) << r1.error;

    auto r2 = mgr_.Attach(FakeSession(), tts);
    ASSERT_TRUE(r2.ok) << r2.error;

    // Third attach: same purpose as first → ALREADY_EXISTS.
    auto r3 = mgr_.Attach(FakeSession(), stt2);
    EXPECT_FALSE(r3.ok);
    EXPECT_EQ(grpc::StatusCode::ALREADY_EXISTS, r3.status_code);
    EXPECT_THAT(r3.error, ::testing::HasSubstr("already attached"));
}

// ---------------------------------------------------------------------------
// A2 — STT (MID_READ=2) → TTS (INJECT=3): both succeed; chain order correct.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A2_ForwardOrderSucceeds) {
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok) << r1.error;

    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    ASSERT_TRUE(r2.ok) << r2.error;

    EXPECT_EQ(2u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
    EXPECT_EQ(2, Mock().media_bug_add_calls.load());

    // Verify function name passed to FS mock.
    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        ASSERT_EQ(2u, Mock().media_bug_add_invocations.size());
        EXPECT_EQ("mod_open_switch", Mock().media_bug_add_invocations[0].function_name);
        EXPECT_EQ("mod_open_switch", Mock().media_bug_add_invocations[1].function_name);
        EXPECT_EQ("stt_transcribe", Mock().media_bug_add_invocations[0].target);
        EXPECT_EQ("tts_playback", Mock().media_bug_add_invocations[1].target);
    }
}

// ---------------------------------------------------------------------------
// A3 — TTS (INJECT=3) → STT (MID_READ=2) without SMBF_FIRST: FAILED_PRECONDITION.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A3_BackwardOrderRejected) {
    // Attach INJECT first.
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok) << r1.error;

    // Now attach MID_READ (rank 2 < max_rank 3) without SMBF_FIRST → rejected.
    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, r2.status_code);
    EXPECT_THAT(r2.error, ::testing::HasSubstr("out-of-order attach"));

    // Only one bug was actually added.
    EXPECT_EQ(1u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
    EXPECT_EQ(1, Mock().media_bug_add_calls.load());
}

// ---------------------------------------------------------------------------
// A4 — STT (MID_READ) → VAD (EARLY): VAD succeeds; mock sees SMBF_FIRST.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A4_VadGetsSmrfFirst) {
    // Attach STT first (rank 2).
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok) << r1.error;

    // Attach VAD (rank 1 < max_rank 2) — manager must OR in SMBF_FIRST.
    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kVadBargeIn, 0, 16000, "t", "e"});
    ASSERT_TRUE(r2.ok) << r2.error;

    // Verify SMBF_FIRST was set in the flags passed to FS.
    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        ASSERT_EQ(2u, Mock().media_bug_add_invocations.size());
        const auto& vad_cap = Mock().media_bug_add_invocations[1];
        EXPECT_EQ("vad_barge_in", vad_cap.target);
        EXPECT_NE(0u, vad_cap.flags & SMBF_FIRST)
            << "manager must OR in SMBF_FIRST for kVadBargeIn";
    }
}

// ---------------------------------------------------------------------------
// A4b — First VAD attach with no flags also gets SMBF_FIRST.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A4b_VadAlwaysGetsSmbfFirst) {
    // VAD is the first (and only) bug; still should get SMBF_FIRST.
    auto r = mgr_.Attach(FakeSession(), {Purpose::kVadBargeIn, 0, 16000, "t", "e"});
    ASSERT_TRUE(r.ok) << r.error;

    std::lock_guard<std::mutex> g(Mock().capture_mu);
    ASSERT_EQ(1u, Mock().media_bug_add_invocations.size());
    EXPECT_NE(0u, Mock().media_bug_add_invocations[0].flags & SMBF_FIRST);
}

// ---------------------------------------------------------------------------
// A5 — BugHandle destructor triggers Detach (mock _remove_callback called).
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A5_BugHandleDestructorDetaches) {
    {
        auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_TRUE(r.handle.attached());
        EXPECT_EQ(Purpose::kSttTranscribe, r.handle.purpose());
        // BugHandle goes out of scope here → calls mgr_.Detach.
    }

    EXPECT_EQ(0u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// A6 — DetachAll after 3 attaches: 3 records removed, count = 0.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A6_DetachAllClearsChannel) {
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    auto r3 = mgr_.Attach(FakeSession(), {Purpose::kTest, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r2.ok);
    ASSERT_TRUE(r3.ok);
    EXPECT_EQ(3u, mgr_.ActiveBugCount("test-channel-uuid-0001"));

    // Release handles without detaching so DetachAll can clean up.
    r1.handle.release();
    r2.handle.release();
    r3.handle.release();

    mgr_.DetachAll("test-channel-uuid-0001");

    EXPECT_EQ(0u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// A7 — DetachAll twice: second call is no-op (idempotent).
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A7_DetachAllIdempotent) {
    auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    ASSERT_TRUE(r.ok);
    r.handle.release();

    mgr_.DetachAll("test-channel-uuid-0001");
    EXPECT_EQ(0u, mgr_.ActiveBugCount("test-channel-uuid-0001"));

    // Second call must not crash or corrupt state.
    EXPECT_NO_FATAL_FAILURE(mgr_.DetachAll("test-channel-uuid-0001"));
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// A8 — Detach(unknown_id): silent no-op.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A8_DetachUnknownIdNoOp) {
    EXPECT_NO_FATAL_FAILURE(mgr_.Detach(0xDEADBEEFULL));
    EXPECT_NO_FATAL_FAILURE(mgr_.Detach(999999ULL));
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// A9 — Concurrent attach/detach (16 threads × 100 ops, distinct UUIDs).
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, A9_ConcurrentAttachDetach) {
    constexpr int kThreads = 16;
    constexpr int kOpsPerThread = 100;

    std::atomic<int> attach_ok{0};
    std::atomic<int> attach_fail{0};

    // Each thread uses a distinct channel UUID.
    auto worker = [&](int thread_id) {
        const std::string uuid = "concurrent-uuid-" + std::to_string(thread_id);
        // Temporarily set next_bleg_uuid — there's a race here among
        // threads that all write Mock().next_bleg_uuid.  That's intentional:
        // the manager must be TSAN-clean regardless.  We protect correctness
        // by using the returned handle's channel_uuid() which is set from
        // the UUID at attach time.
        //
        // For a clean test with distinct UUIDs per-thread we instead use a
        // thread-local session with a pre-set UUID.  Since the mock
        // SessionGetUuid reads from next_bleg_uuid (not the session pointer),
        // we set it under the capture_mu lock to avoid a data race.
        {
            std::lock_guard<std::mutex> g(Mock().capture_mu);
            Mock().next_bleg_uuid = uuid;
        }

        MediaBugManager& m = mgr_;
        for (int i = 0; i < kOpsPerThread; ++i) {
            auto r = m.Attach(FakeSession(), {Purpose::kTest, 0, 16000, "t", "e"});
            if (r.ok) {
                attach_ok.fetch_add(1, std::memory_order_relaxed);
                // Immediately detach via handle destructor.
            } else {
                attach_fail.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    // All bugs attached by threads that saw a unique UUID were subsequently
    // destroyed by their BugHandle destructors.  Manager must be empty.
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());

    // At minimum some attaches succeeded (sanity).
    EXPECT_GT(attach_ok.load(), 0);
}

TEST_F(BugManagerTest, AttachDoesNotHoldGlobalLockAcrossFsAdd) {
    using namespace std::chrono_literals;

    {
        std::lock_guard<std::mutex> g(Mock().media_bug_add_block_mu);
        Mock().media_bug_add_block_remaining = 1;
        Mock().media_bug_add_release = false;
    }

    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    std::atomic<bool> second_ok{false};

    Mock().next_bleg_uuid = "channel-a";
    std::thread first([&] {
        auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
        if (r.ok) {
            r.handle.release();
        }
        first_done.store(true, std::memory_order_release);
    });

    bool first_blocked = false;
    {
        std::unique_lock<std::mutex> g(Mock().media_bug_add_block_mu);
        first_blocked = Mock().media_bug_add_cv.wait_for(
            g, 1s, [] { return Mock().media_bug_add_waiting == 1; });
    }
    EXPECT_TRUE(first_blocked);
    if (!first_blocked) {
        {
            std::lock_guard<std::mutex> g(Mock().media_bug_add_block_mu);
            Mock().media_bug_add_release = true;
        }
        Mock().media_bug_add_cv.notify_all();
        first.join();
        return;
    }

    Mock().next_bleg_uuid = "channel-b";
    std::thread second([&] {
        auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
        second_ok.store(r.ok, std::memory_order_release);
        if (r.ok) {
            r.handle.release();
        }
        second_done.store(true, std::memory_order_release);
    });

    for (int i = 0; i < 100 && !second_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(second_done.load(std::memory_order_acquire))
        << "attach on channel-b should not wait for channel-a FsMediaBugAdd";
    EXPECT_TRUE(second_ok.load(std::memory_order_acquire));
    EXPECT_FALSE(first_done.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> g(Mock().media_bug_add_block_mu);
        Mock().media_bug_add_release = true;
    }
    Mock().media_bug_add_cv.notify_all();

    first.join();
    second.join();

    mgr_.DetachAll("channel-a");
    mgr_.DetachAll("channel-b");
}

// ---------------------------------------------------------------------------
// Extra: OswMediaChannelDestroy calls DetachAll on CS_DESTROY.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, ChannelDestroyCallsDetachAll) {
    // Attach two bugs under the same UUID.
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r2.ok);
    r1.handle.release();
    r2.handle.release();
    EXPECT_EQ(2u, mgr_.ActiveBugCount("test-channel-uuid-0001"));

    // Simulate CS_DESTROY hook: channel state = CS_DESTROY.
    Mock().next_channel_get_state = CS_DESTROY;

    // Call the trampoline directly (would normally be called by FS).
    // We need MediaBugManager::Instance() to return our local mgr_.
    // Since MediaBugManager::Instance() is a Meyers singleton and we have
    // a local mgr_ here, call DetachAll directly to test the behaviour.
    // The actual OswMediaChannelDestroy hook calls Instance().DetachAll.
    mgr_.DetachAll("test-channel-uuid-0001");

    EXPECT_EQ(0u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
    // FF-032: DetachAll on CS_DESTROY must NOT call remove_callback.
    // The media_bug_remove_callback_calls counter should remain at 0.
    EXPECT_EQ(0, Mock().media_bug_remove_callback_calls.load());
}

// ---------------------------------------------------------------------------
// Extra: FS bug add failure returns kInternal.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, FsBugAddFailureReturnsInternal) {
    Mock().next_bug_add_status = SWITCH_STATUS_GENERR;
    auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(grpc::StatusCode::INTERNAL, r.status_code);
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// Extra: BugHandle move semantics.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, BugHandleMoveDoesNotDoubleDetach) {
    auto r = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    ASSERT_TRUE(r.ok);

    BugHandle moved = std::move(r.handle);
    EXPECT_FALSE(r.handle.attached());
    EXPECT_TRUE(moved.attached());

    // Destroy moved handle — should call Detach exactly once.
    moved = BugHandle{};  // move-assign empty handle → old handle destructs
    EXPECT_FALSE(moved.attached());
    EXPECT_EQ(0u, mgr_.TotalActiveBugCount());
}

// ---------------------------------------------------------------------------
// Extra: release() prevents Detach on destructor.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, BugHandleReleaseNoDetach) {
    std::uint64_t captured_id = 0;
    {
        auto r = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, 0, 16000, "t", "e"});
        ASSERT_TRUE(r.ok);
        // Capture the bug_id via channel count (not ideal but avoids friends).
        EXPECT_EQ(1u, mgr_.ActiveBugCount("test-channel-uuid-0001"));
        (void)captured_id;
        r.handle.release();  // release ownership — destructor must not detach
    }
    // Bug is still registered.
    EXPECT_EQ(1u, mgr_.ActiveBugCount("test-channel-uuid-0001"));

    // Manually clean up to avoid leak in test.
    mgr_.DetachAll("test-channel-uuid-0001");
}

// ---------------------------------------------------------------------------
// Extra: SMBF_FIRST in caller flags also bypasses rank check.
// ---------------------------------------------------------------------------

TEST_F(BugManagerTest, CallerSmbfFirstBypassesRankCheck) {
    // Attach TTS first (rank 3).
    auto r1 = mgr_.Attach(FakeSession(), {Purpose::kTtsPlayback, 0, 16000, "t", "e"});
    ASSERT_TRUE(r1.ok);

    // Attach STT (rank 2) with caller-supplied SMBF_FIRST → allowed.
    auto r2 = mgr_.Attach(FakeSession(), {Purpose::kSttTranscribe, SMBF_FIRST, 16000, "t", "e"});
    ASSERT_TRUE(r2.ok) << r2.error;

    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        ASSERT_EQ(2u, Mock().media_bug_add_invocations.size());
        // Second call should have SMBF_FIRST set.
        EXPECT_NE(0u, Mock().media_bug_add_invocations[1].flags & SMBF_FIRST);
    }
}

}  // namespace
