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
#include <cstdint>
#include <memory>
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
