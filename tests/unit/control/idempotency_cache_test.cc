/*
 * tests/unit/control/idempotency_cache_test.cc
 *
 * Unit tests for osw::control::IdempotencyCache (W5 Track B).
 *
 * Coverage:
 *   - Basic LookupOrReserve / Store / Cancel flow.
 *   - LRU eviction at capacity (oldest entry evicted when capacity+1 is
 *     inserted).
 *   - TTL expiry — 1-second TTL with 1100 ms sleep_for (no clock seam
 *     needed; steady_clock is monotonic, so this is deterministic on any
 *     non-pathological test host).
 *   - In-flight wait: thread A reserves (kMiss), thread B calls
 *     LookupOrReserve for the same key (blocks). A calls Store. B
 *     unblocks and observes kHit with A's stored entry.
 *   - In-flight wait timeout: thread A reserves and never stores; thread B's
 *     LookupOrReserve returns kMiss after in_flight_max_wait elapses.
 *   - Concurrent stress: 16 threads × 1000 ops with 8 shared request_ids
 *     (forces contention). Each op: LookupOrReserve → if kMiss, Store.
 *     No crash, no deadlock, no UB (TSAN gate verifies the last property).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/idempotency_cache.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

namespace {

using Cache = osw::control::IdempotencyCache;
using State = Cache::State;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

Cache::Entry MakeEntry(grpc::Status status,
                       const std::string& body,
                       std::chrono::seconds ttl = std::chrono::seconds(300)) {
    Cache::Entry e;
    e.status = std::move(status);
    e.serialized_response = body;
    e.expires_at = std::chrono::steady_clock::now() + ttl;
    return e;
}

// -----------------------------------------------------------------------
// Basic flow
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, EmptyRequestIdBypassesCache) {
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(30));
    auto r = cache.LookupOrReserve("");
    EXPECT_EQ(r.state, State::kMiss);
    // Store + Cancel must not crash on empty id.
    cache.Store("", MakeEntry(grpc::Status::OK, "body"));
    cache.Cancel("");
}

TEST(IdempotencyCacheTest, MissReservesAndStoreHits) {
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(30));
    const std::string key = "req-001";

    // First call → kMiss + reserve.
    auto r1 = cache.LookupOrReserve(key);
    ASSERT_EQ(r1.state, State::kMiss);

    // Store a result.
    cache.Store(key, MakeEntry(grpc::Status::OK, "response-body"));

    // Second call → kHit.
    auto r2 = cache.LookupOrReserve(key);
    ASSERT_EQ(r2.state, State::kHit);
    EXPECT_TRUE(r2.entry.status.ok());
    EXPECT_EQ(r2.entry.serialized_response, "response-body");
}

TEST(IdempotencyCacheTest, CancelReleasesReservationAsMiss) {
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(30));
    const std::string key = "req-002";

    auto r1 = cache.LookupOrReserve(key);
    ASSERT_EQ(r1.state, State::kMiss);

    // Cancel: subsequent call should get a fresh kMiss (no stored entry).
    cache.Cancel(key);

    // The key is no longer in-flight; a new caller can acquire it.
    auto r2 = cache.LookupOrReserve(key);
    EXPECT_EQ(r2.state, State::kMiss);
}

TEST(IdempotencyCacheTest, StoreOverwritesExistingEntry) {
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(30));
    const std::string key = "req-003";

    cache.LookupOrReserve(key);
    cache.Store(key, MakeEntry(grpc::Status::OK, "first"));

    // Second look-up hits. Then "overwrite" with new value via Store directly.
    // (This mimics the timeout-fallback path where a second executor also
    // calls Store — last-write-wins.)
    cache.Store(key, MakeEntry(grpc::Status::OK, "second"));

    auto r = cache.LookupOrReserve(key);
    ASSERT_EQ(r.state, State::kHit);
    EXPECT_EQ(r.entry.serialized_response, "second");
}

// -----------------------------------------------------------------------
// LRU eviction
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, LruEvictionAtCapacity) {
    constexpr std::size_t kCapacity = 4;
    Cache cache(kCapacity, std::chrono::seconds(300), std::chrono::seconds(30));

    // Fill to capacity.
    for (std::size_t i = 0; i < kCapacity; ++i) {
        const std::string key = "k" + std::to_string(i);
        cache.LookupOrReserve(key);
        cache.Store(key, MakeEntry(grpc::Status::OK, "v" + std::to_string(i)));
    }

    // All four should be hits.
    for (std::size_t i = 0; i < kCapacity; ++i) {
        const std::string key = "k" + std::to_string(i);
        EXPECT_EQ(cache.LookupOrReserve(key).state, State::kHit) << "key=" << key;
    }

    // Insert one more — should evict the least-recently-used (k0, which
    // was inserted first and never accessed since).
    const std::string new_key = "k_new";
    cache.LookupOrReserve(new_key);
    cache.Store(new_key, MakeEntry(grpc::Status::OK, "v_new"));

    // k_new must hit.
    EXPECT_EQ(cache.LookupOrReserve(new_key).state, State::kHit);

    // k0 must have been evicted (returns kMiss, not kHit).
    EXPECT_EQ(cache.LookupOrReserve("k0").state, State::kMiss);
}

TEST(IdempotencyCacheTest, LruPromotesOnAccess) {
    // Access k0 after k1 is inserted so k1 is the LRU candidate.
    constexpr std::size_t kCapacity = 3;
    Cache cache(kCapacity, std::chrono::seconds(300), std::chrono::seconds(30));

    // Insert k0, k1, k2.
    for (auto* k : {"k0", "k1", "k2"}) {
        cache.LookupOrReserve(k);
        cache.Store(k, MakeEntry(grpc::Status::OK, k));
    }

    // Access k0 (promotes it to most-recently-used).
    cache.LookupOrReserve("k0");

    // Insert k3 — must evict k1 (now LRU), not k0.
    cache.LookupOrReserve("k3");
    cache.Store("k3", MakeEntry(grpc::Status::OK, "k3"));

    EXPECT_EQ(cache.LookupOrReserve("k0").state, State::kHit);
    EXPECT_EQ(cache.LookupOrReserve("k1").state, State::kMiss);
    EXPECT_EQ(cache.LookupOrReserve("k2").state, State::kHit);
    EXPECT_EQ(cache.LookupOrReserve("k3").state, State::kHit);
}

// -----------------------------------------------------------------------
// TTL expiry
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, TtlExpiry) {
    // 1-second TTL. We sleep 1100 ms to let the entry expire.
    Cache cache(16, std::chrono::seconds(1), std::chrono::seconds(30));
    const std::string key = "req-ttl";

    cache.LookupOrReserve(key);
    cache.Store(key, MakeEntry(grpc::Status::OK, "body", std::chrono::seconds(1)));

    // Immediately after store — should be a hit.
    EXPECT_EQ(cache.LookupOrReserve(key).state, State::kHit);

    // Wait for TTL to elapse.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Entry should now be expired → kMiss.
    EXPECT_EQ(cache.LookupOrReserve(key).state, State::kMiss);
}

// -----------------------------------------------------------------------
// In-flight wait: thread A reserves, thread B waits, A stores, B hits
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, InFlightWaitThenHit) {
    // Use a long in_flight_max_wait (30s) so the test doesn't time out.
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(30));
    const std::string key = "req-inflight";

    // Thread A: reserve the key (kMiss).
    auto ra = cache.LookupOrReserve(key);
    ASSERT_EQ(ra.state, State::kMiss);

    std::atomic<State> b_state{State::kMiss};
    std::atomic<bool> b_started{false};

    // Thread B: will block until A stores.
    std::thread b([&] {
        b_started.store(true, std::memory_order_release);
        auto rb = cache.LookupOrReserve(key);
        b_state.store(rb.state, std::memory_order_release);
    });

    // Wait for B to be inside LookupOrReserve.
    while (!b_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Give B time to block on the condvar.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // A stores the result.
    cache.Store(key, MakeEntry(grpc::Status::OK, "result-body"));

    b.join();

    // B must have woken up and observed kHit.
    EXPECT_EQ(b_state.load(), State::kHit);
}

// -----------------------------------------------------------------------
// In-flight wait timeout
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, InFlightWaitTimeout) {
    // Use a 200 ms in_flight_max_wait so the test completes quickly.
    Cache cache(16, std::chrono::seconds(300), std::chrono::seconds(0));
    // 0 seconds → waiter returns immediately (condvar deadline already
    // in the past).  This is the "never stores" scenario.
    const std::string key = "req-timeout";

    // Thread A: reserve the key (kMiss); will NEVER call Store.
    auto ra = cache.LookupOrReserve(key);
    ASSERT_EQ(ra.state, State::kMiss);

    // Thread B: waits, times out, returns kMiss.
    auto rb = cache.LookupOrReserve(key);
    EXPECT_EQ(rb.state, State::kMiss);

    // Clean up A's slot.
    cache.Cancel(key);
}

TEST(IdempotencyCacheTest, InFlightWaitTimeoutShort) {
    // 200 ms timeout — enough for a real wait but fast enough for CI.
    Cache cache(16, std::chrono::seconds(300), std::chrono::milliseconds(200));
    const std::string key = "req-timeout-200ms";

    auto ra = cache.LookupOrReserve(key);
    ASSERT_EQ(ra.state, State::kMiss);

    // B will wait ~200ms then return kMiss.
    const auto t0 = std::chrono::steady_clock::now();
    auto rb = cache.LookupOrReserve(key);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(rb.state, State::kMiss);
    // Sanity: waited at least 150 ms (generous lower bound; CI can be slow).
    EXPECT_GE(elapsed, std::chrono::milliseconds(150));

    cache.Cancel(key);
}

// -----------------------------------------------------------------------
// Concurrent stress
// -----------------------------------------------------------------------

TEST(IdempotencyCacheTest, ConcurrentStress) {
    // 16 threads × 1000 ops. 8 shared keys → high contention per key.
    // Each thread: LookupOrReserve → if kMiss, tiny sleep, then Store.
    // Goal: no crash, no deadlock, no data race (TSAN CI job verifies).
    constexpr int kThreads = 16;
    constexpr int kOps = 1000;
    constexpr int kKeys = 8;

    Cache cache(64, std::chrono::seconds(300), std::chrono::seconds(5));

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOps; ++i) {
                const std::string key = "stress-key-" + std::to_string((t * kOps + i) % kKeys);
                auto result = cache.LookupOrReserve(key);
                if (result.state == Cache::State::kMiss) {
                    // Simulate brief execution time.
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    cache.Store(
                        key, MakeEntry(grpc::Status::OK, "body-" + key, std::chrono::seconds(1)));
                }
                // kHit: nothing to do.
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // If we reach here without a crash or deadlock, the test passes.
    SUCCEED();
}

}  // namespace
