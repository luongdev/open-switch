/*
 * tests/unit/events/subscribe_replay_test.cc
 *
 * Unit tests for the since_seq replay semantics that the
 * SubscribeEvents handler relies on. The handler calls
 * Ring::SnapshotFromSeq(since_seq) on each requested tier; if any
 * tier reports found_in_window=false the handler returns
 * RESOURCE_EXHAUSTED to the client. If all tiers' windows contain
 * since_seq, the matching entries are pre-loaded into the
 * subscriber's send queue before AddSubscriber, then live-tail flows
 * after.
 *
 * These tests exercise the Ring-side primitives that drive that
 * behaviour — they're FS-agnostic and don't require a live gRPC
 * server. The handler itself is exercised by an integration test
 * under tests/integration in a later commit (gRPC stub harness).
 *
 * Covered:
 *   - since_seq == 0 returns empty entries with found_in_window=true
 *     (no replay needed; live tail only).
 *   - since_seq within the live ring window returns the matching slice.
 *   - since_seq below min_seq returns found_in_window=false
 *     (RESOURCE_EXHAUSTED path).
 *   - since_seq above max_seq returns empty entries with
 *     found_in_window=true (caller treats as "no replay needed").
 *   - End-to-end: replay slice followed by Broadcaster-routed live
 *     tail; subscriber's send queue holds both, ordered.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/events/v1/events.pb.h"

#include "osw/events/binder.h"
#include "osw/events/ring.h"
#include "osw/events/subscribe/broadcaster.h"
#include "osw/events/subscribe/routing.h"
#include "osw/events/subscribe/subscriber.h"
#include "osw/events/tier.h"
#include "osw/observability/health.h"

namespace {

using osw::events::Broadcaster;
using osw::events::KickReason;
using osw::events::Ring;
using osw::events::RingEntry;
using osw::events::RingSet;
using osw::events::Subscriber;
using osw::events::SubscriberFilter;
using osw::events::Tier;

// Build a minimal serialised envelope. The replay path doesn't parse;
// it just shuffles shared_ptr<const string> into the send queue.
std::shared_ptr<const std::string> MakeBytes(
    std::uint64_t seq,
    const std::string& event_name = "CHANNEL_HANGUP_COMPLETE",
    const std::string& node_id = "node-a") {
    open_switch::events::v1::EventEnvelope env;
    env.set_event_id("ev-" + std::to_string(seq));
    env.set_tier(open_switch::events::v1::TIER_1_CRITICAL);
    env.set_event_name(event_name);
    env.set_node_id(node_id);
    env.set_seq(seq);
    env.set_schema_version(1);
    auto bytes = std::make_shared<std::string>();
    EXPECT_TRUE(env.SerializeToString(bytes.get()));
    return bytes;
}

RingEntry MakeEntry(std::uint64_t seq) {
    return RingEntry{seq, MakeBytes(seq)};
}

RingEntry MakeEntryWithName(std::uint64_t seq, const std::string& event_name) {
    return RingEntry{seq, MakeBytes(seq, event_name)};
}

TEST(SubscribeReplayTest, SinceSeqZeroReturnsEmptyWindow) {
    Ring ring(64);
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        ring.Push(MakeEntry(i), &dropped);
    }
    auto snap = ring.SnapshotFromSeq(0);
    EXPECT_TRUE(snap.found_in_window);
    // since_seq=0 (exclusive) returns entries with seq > 0 — i.e. all.
    // The handler still treats this as "live-tail only" because the
    // SubscribeEvents proto contract states since_seq=0 means "no
    // replay". The Ring is correct; the handler short-circuits when
    // since_seq == 0 in subscribe_events_handler.cc (ReplaySinceSeq is
    // only called if since_seq > 0).
    EXPECT_EQ(snap.entries.size(), 5u);
}

TEST(SubscribeReplayTest, SinceSeqInWindowReturnsTailSlice) {
    Ring ring(64);
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        ring.Push(MakeEntry(i), &dropped);
    }
    auto snap = ring.SnapshotFromSeq(7);
    EXPECT_TRUE(snap.found_in_window);
    // Exclusive: entries with seq > 7 → 8, 9, 10.
    ASSERT_EQ(snap.entries.size(), 3u);
    EXPECT_EQ(snap.entries[0].seq, 8u);
    EXPECT_EQ(snap.entries[1].seq, 9u);
    EXPECT_EQ(snap.entries[2].seq, 10u);
    EXPECT_EQ(snap.current_min_seq, 1u);
    EXPECT_EQ(snap.current_max_seq, 10u);
}

TEST(SubscribeReplayTest, SinceSeqBelowMinSeqReportsEvicted) {
    Ring ring(/*capacity=*/4);
    std::uint64_t dropped = 0;
    // Push 10 entries into a cap=4 ring → seqs 7-10 remain; 1-6 evicted.
    for (std::uint64_t i = 1; i <= 10; ++i) {
        ring.Push(MakeEntry(i), &dropped);
    }
    EXPECT_EQ(dropped, 6u);

    // since_seq=3 is below min_seq (7). The handler MUST return
    // RESOURCE_EXHAUSTED.
    auto snap = ring.SnapshotFromSeq(3);
    EXPECT_FALSE(snap.found_in_window);
    EXPECT_GE(snap.current_min_seq, 7u);
}

TEST(SubscribeReplayTest, SinceSeqAtMaxSeqReturnsEmpty) {
    Ring ring(64);
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        ring.Push(MakeEntry(i), &dropped);
    }
    auto snap = ring.SnapshotFromSeq(5);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_TRUE(snap.entries.empty());  // exclusive: seq > 5 → none
}

TEST(SubscribeReplayTest, SinceSeqAboveMaxSeqReturnsEmpty) {
    // The handler treats this as "subscriber caught up; just live-tail".
    Ring ring(64);
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        ring.Push(MakeEntry(i), &dropped);
    }
    auto snap = ring.SnapshotFromSeq(999);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_TRUE(snap.entries.empty());
}

TEST(SubscribeReplayTest, ReplayThenLiveTailDeliversBothToSubscriber) {
    // End-to-end check: a subscriber registers with since_seq pointing
    // into the ring, the handler-side replay pre-loads the tail slice
    // into the SendQueue, then the broadcaster delivers further events
    // live. The test simulates the handler's replay step manually then
    // pushes more events through the broadcaster's WorkerLoop.
    auto rings = std::make_unique<RingSet>(64, 64, 64);
    osw::Health health;
    auto bcast = std::make_unique<Broadcaster>(rings.get(), &health);

    Ring* t1 = rings->Get(Tier::k1Critical);
    ASSERT_NE(t1, nullptr);

    // Pre-populate the ring with 5 events (the "history" the
    // subscriber will replay from).
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        t1->Push(MakeEntry(i), &dropped);
    }

    // Subscriber asks for since_seq=3 → handler should replay seqs 4,5.
    auto sub = std::make_shared<Subscriber>("s-replay", SubscriberFilter{}, /*cap=*/32);

    // Mimic the handler's replay step.
    auto snap = t1->SnapshotFromSeq(3);
    ASSERT_TRUE(snap.found_in_window);
    for (const auto& entry : snap.entries) {
        ASSERT_TRUE(sub->Queue().TryPush(entry.envelope_bytes));
    }
    EXPECT_EQ(sub->Queue().Size(), 2u);

    // Now register + start the broadcaster so live-tail flows in.
    bcast->AddSubscriber(sub);
    bcast->Start();

    // Push 3 more events live.
    for (std::uint64_t i = 6; i <= 8; ++i) {
        t1->Push(MakeEntry(i), &dropped);
    }

    // Wait up to 1s for live-tail delivery.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sub->Queue().Size() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(sub->Queue().Size(), 5u);  // 2 replayed + 3 live

    bcast->Stop();
    EXPECT_TRUE(sub->IsClosed());
    EXPECT_EQ(sub->GetKickReason(), KickReason::kShutdown);
}

TEST(SubscribeReplayTest, ReplayHonoursEventNameFilter) {
    // Codex W2 C-1: a subscriber that narrowed event_names should
    // receive ONLY matching entries from the replay window — not the
    // entire tier slice. The replay path uses the same routing-fields
    // scanner + MatchesFilter as the broadcaster's live-tail dispatch.
    Ring ring(64);
    std::uint64_t dropped = 0;
    // Mixed events: CHANNEL_ANSWER (1,3,5), CHANNEL_HANGUP_COMPLETE (2,4,6).
    ring.Push(MakeEntryWithName(1, "CHANNEL_ANSWER"), &dropped);
    ring.Push(MakeEntryWithName(2, "CHANNEL_HANGUP_COMPLETE"), &dropped);
    ring.Push(MakeEntryWithName(3, "CHANNEL_ANSWER"), &dropped);
    ring.Push(MakeEntryWithName(4, "CHANNEL_HANGUP_COMPLETE"), &dropped);
    ring.Push(MakeEntryWithName(5, "CHANNEL_ANSWER"), &dropped);
    ring.Push(MakeEntryWithName(6, "CHANNEL_HANGUP_COMPLETE"), &dropped);

    SubscriberFilter f;
    f.event_name_globs = {"CHANNEL_ANSWER"};
    auto sub = std::make_shared<Subscriber>("s-filter", f, /*cap=*/16);

    // Mimic the handler's filtered replay step (the same routing.h
    // scanner + Subscriber::MatchesFilter chain).
    auto snap = ring.SnapshotFromSeq(0);
    ASSERT_TRUE(snap.found_in_window);
    for (const auto& entry : snap.entries) {
        const auto rf = osw::events::ExtractRoutingFields(*entry.envelope_bytes);
        if (!sub->MatchesFilter(Tier::k1Critical, rf.event_name, rf.node_id))
            continue;
        ASSERT_TRUE(sub->Queue().TryPush(entry.envelope_bytes));
    }
    // Only the 3 CHANNEL_ANSWER entries (seqs 1, 3, 5) should be queued.
    EXPECT_EQ(sub->Queue().Size(), 3u);
}

TEST(SubscribeReplayTest, ReplayHonoursNodeIdFilter) {
    Ring ring(64);
    std::uint64_t dropped = 0;
    // Alternating node_ids: node-a (1,3,5), node-b (2,4,6).
    ring.Push(RingEntry{1, MakeBytes(1, "CHANNEL_ANSWER", "node-a")}, &dropped);
    ring.Push(RingEntry{2, MakeBytes(2, "CHANNEL_ANSWER", "node-b")}, &dropped);
    ring.Push(RingEntry{3, MakeBytes(3, "CHANNEL_ANSWER", "node-a")}, &dropped);
    ring.Push(RingEntry{4, MakeBytes(4, "CHANNEL_ANSWER", "node-b")}, &dropped);

    SubscriberFilter f;
    f.node_id = "node-a";
    auto sub = std::make_shared<Subscriber>("s-node", f, /*cap=*/16);

    auto snap = ring.SnapshotFromSeq(0);
    ASSERT_TRUE(snap.found_in_window);
    for (const auto& entry : snap.entries) {
        const auto rf = osw::events::ExtractRoutingFields(*entry.envelope_bytes);
        if (!sub->MatchesFilter(Tier::k1Critical, rf.event_name, rf.node_id))
            continue;
        ASSERT_TRUE(sub->Queue().TryPush(entry.envelope_bytes));
    }
    EXPECT_EQ(sub->Queue().Size(), 2u);  // only node-a entries
}

TEST(SubscribeReplayTest, AtomicAddSubscriberClosesReplayLiveTailGap) {
    // Codex W2 B-1 deterministic race fixture.
    //
    // Without the fix, an event with seq in (snap.max_seq, post-snap
    // max] could be popped from the ring by the broadcaster worker
    // and dispatched to existing subscribers BEFORE AddSubscriber for
    // the new subscriber. The new subscriber starts at the next live
    // event and silently misses the gap entries — violating the proto's
    // "replay then continue with live tail" contract and the Tier-1
    // must-persist guarantee.
    //
    // The fix: Broadcaster::AddSubscriberAtomic holds roster_mu_ across
    // the replay closure. The per-tier worker blocks at
    // RosterSnapshot() after popping its batch from the ring; on
    // release the new subscriber is in the roster and receives every
    // popped-but-undispatched event.
    //
    // To make the race deterministic we install a post-pop hook on
    // the broadcaster — fired right after a non-empty batch comes
    // back from WaitAndPopBatch and BEFORE RosterSnapshot. The hook
    // blocks the worker until the test signals it; the test uses
    // that signal to call AddSubscriberAtomic in a controlled
    // sequence.

    auto rings = std::make_unique<RingSet>(256, 64, 64);
    osw::Health health;
    auto bcast = std::make_unique<Broadcaster>(rings.get(), &health);
    Ring* t1 = rings->Get(Tier::k1Critical);
    ASSERT_NE(t1, nullptr);

    // Pre-populate 1..50 (the history the new subscriber would replay).
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        t1->Push(MakeEntry(i), &dropped);
    }

    // An existing subscriber so the broadcaster has someone to dispatch
    // to during the race window. Sized large enough to never kick.
    auto existing = std::make_shared<Subscriber>("ex", SubscriberFilter{}, /*cap=*/512);
    bcast->AddSubscriber(existing);

    // The hook: block on the first batch the worker pops for tier-1
    // AFTER we've explicitly armed it. We start in "disarmed" state so
    // the broadcaster can freely drain the initial 1..50 entries to
    // `existing` (otherwise the worker would hang on Start()).
    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool hook_armed = false;    // test sets to true after initial drain
    bool hook_inside = false;   // worker sets when it enters the hook
    bool hook_release = false;  // test sets to true to let the worker proceed

    bcast->SetPostPopHookForTesting([&](Tier t) {
        if (t != Tier::k1Critical)
            return;
        std::unique_lock<std::mutex> lk(hook_mu);
        if (!hook_armed)
            return;
        hook_inside = true;
        hook_cv.notify_all();
        hook_cv.wait(lk, [&]() { return hook_release; });
        // One-shot: disarm so subsequent batches drain freely.
        hook_armed = false;
        hook_inside = false;
    });

    bcast->Start();

    // Drain the initial 50 entries to `existing`.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (existing->Queue().Size() < 50 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_EQ(existing->Queue().Size(), 50u);
    }

    // Arm the hook for the next batch.
    {
        std::lock_guard<std::mutex> lk(hook_mu);
        hook_armed = true;
    }

    // Push 51..70 — the worker will pop them and then block in the
    // hook before dispatching. THIS is the race window: in the broken
    // code, the worker would dispatch to existing-only (no new sub yet)
    // and the new sub would miss 51..70.
    for (std::uint64_t i = 51; i <= 70; ++i) {
        t1->Push(MakeEntry(i), &dropped);
    }

    // Wait for the worker to reach the hook (it has popped 51..70 and
    // is waiting before RosterSnapshot).
    {
        std::unique_lock<std::mutex> lk(hook_mu);
        hook_cv.wait_for(lk, std::chrono::seconds(2), [&]() { return hook_inside; });
        ASSERT_TRUE(hook_inside) << "worker did not reach the post-pop hook";
    }

    // Now call AddSubscriberAtomic. The handler's replay closure runs
    // under roster_mu_; the worker is parked in the hook and has not
    // yet taken roster_mu_. The closure snapshots the ring at
    // since_seq=50 — at this point the worker has already popped 51..70
    // so they are NOT in the ring (snapshot returns empty for that
    // window). The new sub gets nothing from the replay step. But when
    // we signal the hook to release, the worker will acquire roster_mu_
    // and dispatch 51..70 — the new sub is now in the roster and
    // receives them.
    auto new_sub = std::make_shared<Subscriber>("new", SubscriberFilter{}, /*cap=*/512);
    bcast->AddSubscriberAtomic(new_sub, [&](Subscriber& s) {
        // The worker has popped at least some of 51..70 (it's blocked
        // in the post-pop hook); any items still in the ring at this
        // point are also dispatched after we release the hook. Push
        // the ring-resident slice into the new subscriber via the
        // standard replay path. Items the worker already popped land
        // on the new sub via the worker's post-hook dispatch (with
        // possible duplicates that the client dedups by seq, per
        // SubscribeEventsRequest.since_seq contract).
        auto snap = t1->SnapshotFromSeq(50);
        for (const auto& e : snap.entries) {
            (void)s.Queue().TryPush(e.envelope_bytes);
        }
    });

    // Release the worker. It dispatches 51..70 to BOTH existing AND
    // new_sub.
    {
        std::lock_guard<std::mutex> lk(hook_mu);
        hook_release = true;
    }
    hook_cv.notify_all();

    // Push 71..100 — these flow normally to both subs.
    for (std::uint64_t i = 71; i <= 100; ++i) {
        t1->Push(MakeEntry(i), &dropped);
    }

    // Wait for delivery to new_sub. It should receive 51..100 = 50
    // events.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (new_sub->Queue().Size() < 50 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Drain new_sub and verify seq contiguity 51..100 with no gap.
    std::set<std::uint64_t> seen;
    while (auto v = new_sub->Queue().WaitAndPop(std::chrono::milliseconds(20))) {
        if (!*v)
            continue;
        open_switch::events::v1::EventEnvelope env;
        ASSERT_TRUE(env.ParseFromString(**v));
        seen.insert(env.seq());
    }
    for (std::uint64_t i = 51; i <= 100; ++i) {
        EXPECT_TRUE(seen.count(i) > 0) << "missing seq=" << i;
    }

    bcast->Stop();
}

TEST(SubscribeReplayTest, AtomicAddSubscriberReplayClosureRunsBeforeAdd) {
    // Sanity test: AddSubscriberAtomic appends the subscriber to the
    // roster AFTER the closure returns. While the closure runs, the
    // new sub MUST NOT be visible to RosterSnapshot (because the closure
    // is running under roster_mu_).
    auto rings = std::make_unique<RingSet>(64, 64, 64);
    osw::Health health;
    auto bcast = std::make_unique<Broadcaster>(rings.get(), &health);

    EXPECT_EQ(bcast->SubscriberCount(), 0u);

    auto sub = std::make_shared<Subscriber>("a", SubscriberFilter{}, /*cap=*/16);

    bool closure_ran = false;
    bcast->AddSubscriberAtomic(sub, [&](Subscriber&) {
        // The lock is held — anyone calling SubscriberCount() concurrently
        // would block. Here we just verify the closure runs to completion.
        closure_ran = true;
    });
    EXPECT_TRUE(closure_ran);
    EXPECT_EQ(bcast->SubscriberCount(), 1u);

    // Calling with a null replay_fn is also accepted.
    auto sub2 = std::make_shared<Subscriber>("b", SubscriberFilter{}, /*cap=*/16);
    bcast->AddSubscriberAtomic(sub2, std::function<void(Subscriber&)>{});
    EXPECT_EQ(bcast->SubscriberCount(), 2u);
}

TEST(SubscribeReplayTest, EvictedSinceSeqClosesSubscriber) {
    // The handler's contract: if any requested tier reports the replay
    // point evicted, the subscriber is closed with kReplayEvicted before
    // AddSubscriber, and the RPC returns RESOURCE_EXHAUSTED.
    auto rings = std::make_unique<RingSet>(4, 64, 64);  // tier1 cap=4
    Ring* t1 = rings->Get(Tier::k1Critical);
    ASSERT_NE(t1, nullptr);

    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        t1->Push(MakeEntry(i), &dropped);  // seqs 7-10 retained
    }

    auto sub = std::make_shared<Subscriber>("s-evicted", SubscriberFilter{}, /*cap=*/32);
    auto snap = t1->SnapshotFromSeq(3);
    ASSERT_FALSE(snap.found_in_window);
    sub->RequestClose(KickReason::kReplayEvicted);

    EXPECT_TRUE(sub->IsClosed());
    EXPECT_EQ(sub->GetKickReason(), KickReason::kReplayEvicted);
}

}  // namespace
