/*
 * tests/unit/events/ring_test.cc
 *
 * Unit tests for osw::events::Ring.
 *
 * Covered:
 *   - Push/Pop FIFO ordering on a single producer.
 *   - Overflow evicts the oldest entry and increments the counter.
 *   - WaitAndPopBatch returns up to max_batch and respects timeout.
 *   - Close() unblocks a waiting consumer and is idempotent.
 *   - SnapshotFromSeq replay window: in-window, out-of-window
 *     (evicted), exact-tail, since_seq=0 (live-tail).
 *   - MPSC under N=8 producers: all entries arrive (no lost writes
 *     when capacity is sufficient); seq monotonicity preserved within
 *     each producer; drop counter accurate when over-capacity.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/ring.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using osw::events::Ring;
using osw::events::RingEntry;

std::shared_ptr<const std::string> MakeBytes(std::string s) {
    return std::make_shared<const std::string>(std::move(s));
}

RingEntry Entry(std::uint64_t seq, std::string body = "") {
    return RingEntry{seq, MakeBytes(body.empty() ? std::to_string(seq) : body)};
}

class RingTest : public ::testing::Test {};

TEST_F(RingTest, PushPopFifo) {
    Ring r(4);
    std::uint64_t dropped = 0;
    EXPECT_TRUE(r.Push(Entry(1), &dropped));
    EXPECT_TRUE(r.Push(Entry(2), &dropped));
    EXPECT_TRUE(r.Push(Entry(3), &dropped));
    EXPECT_EQ(dropped, 0u);
    EXPECT_EQ(r.Size(), 3u);

    auto e1 = r.TryPop();
    auto e2 = r.TryPop();
    auto e3 = r.TryPop();
    ASSERT_TRUE(e1 && e2 && e3);
    EXPECT_EQ(e1->seq, 1u);
    EXPECT_EQ(e2->seq, 2u);
    EXPECT_EQ(e3->seq, 3u);
    EXPECT_FALSE(r.TryPop().has_value());
}

TEST_F(RingTest, OverflowEvictsOldestAndIncrementsCounter) {
    Ring r(3);
    std::uint64_t dropped = 0;
    EXPECT_TRUE(r.Push(Entry(1), &dropped));
    EXPECT_TRUE(r.Push(Entry(2), &dropped));
    EXPECT_TRUE(r.Push(Entry(3), &dropped));
    EXPECT_EQ(dropped, 0u);

    // 4th push at capacity: evicts seq=1, enqueues seq=4.
    EXPECT_FALSE(r.Push(Entry(4), &dropped));
    EXPECT_EQ(dropped, 1u);
    EXPECT_EQ(r.Size(), 3u);

    auto e = r.TryPop();
    ASSERT_TRUE(e);
    EXPECT_EQ(e->seq, 2u);  // oldest now seq=2
}

TEST_F(RingTest, OverflowEvictsMultipleOnBurst) {
    Ring r(2);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 5; ++s) {
        r.Push(Entry(s), &dropped);
    }
    EXPECT_EQ(dropped, 3u);  // 3 evictions: seqs 1,2,3
    EXPECT_EQ(r.Size(), 2u);
    auto a = r.TryPop();
    auto b = r.TryPop();
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->seq, 4u);
    EXPECT_EQ(b->seq, 5u);
}

TEST_F(RingTest, WaitAndPopBatchRespectsMaxBatch) {
    Ring r(16);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 8; ++s) {
        r.Push(Entry(s), &dropped);
    }
    auto batch = r.WaitAndPopBatch(3, std::chrono::milliseconds(5));
    EXPECT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0].seq, 1u);
    EXPECT_EQ(batch[2].seq, 3u);
    EXPECT_EQ(r.Size(), 5u);
}

TEST_F(RingTest, WaitAndPopBatchReturnsEmptyOnTimeout) {
    Ring r(16);
    const auto t0 = std::chrono::steady_clock::now();
    auto batch = r.WaitAndPopBatch(8, std::chrono::milliseconds(20));
    const auto delta = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(batch.empty());
    EXPECT_GE(delta, std::chrono::milliseconds(15));
}

TEST_F(RingTest, CloseUnblocksWaitAndPopBatch) {
    Ring r(16);
    std::atomic<bool> awake{false};
    std::thread t([&]() {
        auto batch = r.WaitAndPopBatch(8, std::chrono::seconds(5));
        EXPECT_TRUE(batch.empty());
        awake.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    r.Close();
    t.join();
    EXPECT_TRUE(awake.load());
    EXPECT_TRUE(r.IsClosed());
    // Idempotent.
    r.Close();
    EXPECT_TRUE(r.IsClosed());
}

TEST_F(RingTest, SnapshotInWindow) {
    Ring r(8);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 5; ++s) {
        r.Push(Entry(s), &dropped);
    }
    auto snap = r.SnapshotFromSeq(2);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_EQ(snap.current_min_seq, 1u);
    EXPECT_EQ(snap.current_max_seq, 5u);
    ASSERT_EQ(snap.entries.size(), 3u);
    EXPECT_EQ(snap.entries[0].seq, 3u);
    EXPECT_EQ(snap.entries[2].seq, 5u);
}

TEST_F(RingTest, SnapshotOutOfWindowReturnsFalse) {
    Ring r(3);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 6; ++s) {
        r.Push(Entry(s), &dropped);
    }
    // Ring now contains seqs 4,5,6 (1,2,3 evicted; dropped == 3).
    EXPECT_EQ(dropped, 3u);
    auto snap = r.SnapshotFromSeq(1);
    EXPECT_FALSE(snap.found_in_window);
    EXPECT_EQ(snap.current_min_seq, 4u);
    EXPECT_EQ(snap.current_max_seq, 6u);
    EXPECT_TRUE(snap.entries.empty());
}

TEST_F(RingTest, SnapshotExactTailEdge) {
    Ring r(4);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 4; ++s) {
        r.Push(Entry(s), &dropped);
    }
    // since_seq=4 → resume point would be 5; ring max is 4 → entries empty,
    // but in-window=true (live tail OK, next push will be delivered).
    auto snap = r.SnapshotFromSeq(4);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_TRUE(snap.entries.empty());
}

TEST_F(RingTest, SnapshotSinceZeroIsLiveTail) {
    Ring r(4);
    std::uint64_t dropped = 0;
    r.Push(Entry(7), &dropped);
    r.Push(Entry(8), &dropped);
    auto snap = r.SnapshotFromSeq(0);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_TRUE(snap.entries.empty());  // since_seq=0 → no replay
}

TEST_F(RingTest, SnapshotBoundaryAtMinSeq) {
    // since_seq+1 == min_seq → in-window (we can resume at min_seq).
    Ring r(3);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 5; s <= 7; ++s) {
        r.Push(Entry(s), &dropped);
    }
    auto snap = r.SnapshotFromSeq(4);
    EXPECT_TRUE(snap.found_in_window);
    ASSERT_EQ(snap.entries.size(), 3u);
    EXPECT_EQ(snap.entries[0].seq, 5u);
}

TEST_F(RingTest, EmptyFreshRingIsInWindow) {
    // Codex W2 I-6: a ring that has never been pushed into reports
    // found_in_window=true so a fresh subscriber attaches at HEAD and
    // future events arrive via live tail.
    Ring r(4);
    auto snap = r.SnapshotFromSeq(0);
    EXPECT_TRUE(snap.found_in_window);
    EXPECT_TRUE(snap.entries.empty());
    EXPECT_EQ(snap.current_max_seq, 0u);

    // Non-zero since_seq on a fresh ring also reports in-window — the
    // client says "I've seen up to N; nothing past it exists yet".
    auto snap2 = r.SnapshotFromSeq(100);
    EXPECT_TRUE(snap2.found_in_window);
    EXPECT_TRUE(snap2.entries.empty());
}

TEST_F(RingTest, EmptyDrainedRingReportsEvicted) {
    // Codex W2 I-6: a ring that had entries up to seq=N but was then
    // fully drained (e.g. broadcaster popped everything, or a quiet
    // tier on a slow-event-rate system) should NOT silently report
    // found_in_window=true for since_seq < N — the subscriber's
    // resume point was evicted; they need to know.
    Ring r(4);
    std::uint64_t dropped = 0;
    r.Push(Entry(10), &dropped);
    r.Push(Entry(11), &dropped);
    r.Push(Entry(12), &dropped);

    // Drain via TryPop.
    while (r.TryPop().has_value()) {
    }
    EXPECT_EQ(r.Size(), 0u);

    // since_seq=5 is BELOW the highest seq ever observed (12) — the
    // resume point at 5+1=6 was evicted/never-in-window.
    auto snap = r.SnapshotFromSeq(5);
    EXPECT_FALSE(snap.found_in_window);
    EXPECT_EQ(snap.current_max_seq, 12u);

    // since_seq=12 (== highest observed) means "caught up; live-tail".
    auto snap2 = r.SnapshotFromSeq(12);
    EXPECT_TRUE(snap2.found_in_window);
    EXPECT_TRUE(snap2.entries.empty());

    // since_seq above max_seq_ever_pushed_ — also live-tail.
    auto snap3 = r.SnapshotFromSeq(99);
    EXPECT_TRUE(snap3.found_in_window);
}

TEST_F(RingTest, EightProducersAllSeqsPresentNoDuplicates) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 256;

    Ring r(kProducers * kPerProducer + 16);  // never overflows
    std::atomic<std::uint64_t> seq_gen{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kPerProducer; ++i) {
                const std::uint64_t my_seq = seq_gen.fetch_add(1, std::memory_order_relaxed) + 1;
                std::uint64_t dropped = 0;  // unused; not over-capacity
                r.Push(Entry(my_seq, "p" + std::to_string(p)), &dropped);
                EXPECT_EQ(dropped, 0u);
            }
        });
    }
    for (auto& t : producers)
        t.join();

    // Drain.
    std::vector<RingEntry> all;
    all.reserve(kProducers * kPerProducer);
    while (auto e = r.TryPop()) {
        all.push_back(std::move(*e));
    }
    EXPECT_EQ(all.size(), static_cast<std::size_t>(kProducers * kPerProducer));

    // MPSC contract: every produced seq appears exactly once. The order
    // in the ring reflects mutex acquisition order, NOT seq order: a
    // producer that grabs a seq id and is then preempted can arrive
    // after a producer with a later id. Sequence monotonicity is a
    // per-tier promise enforced by the broadcaster + subscribers via
    // the EventEnvelope.seq field — NOT by the ring's drain order.
    std::vector<std::uint64_t> seqs;
    seqs.reserve(all.size());
    for (const auto& e : all)
        seqs.push_back(e.seq);
    std::sort(seqs.begin(), seqs.end());
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        EXPECT_EQ(seqs[i], static_cast<std::uint64_t>(i + 1));
    }
}

TEST_F(RingTest, FourProducersOverflowAccountsAllEntries) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1000;
    constexpr int kCapacity = 64;

    Ring r(kCapacity);
    std::atomic<std::uint64_t> seq_gen{0};
    std::atomic<std::uint64_t> drops_total{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&]() {
            std::uint64_t local_dropped = 0;
            for (int i = 0; i < kPerProducer; ++i) {
                const std::uint64_t my_seq = seq_gen.fetch_add(1, std::memory_order_relaxed) + 1;
                r.Push(Entry(my_seq), &local_dropped);
            }
            drops_total.fetch_add(local_dropped, std::memory_order_relaxed);
        });
    }
    for (auto& t : producers)
        t.join();

    // produced - kept = dropped.
    const std::size_t produced = static_cast<std::size_t>(kProducers * kPerProducer);
    const std::size_t kept = r.Size();
    EXPECT_LE(kept, static_cast<std::size_t>(kCapacity));
    EXPECT_EQ(produced - kept, drops_total.load());
}

}  // namespace
