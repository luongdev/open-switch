/*
 * include/osw/events/ring.h
 *
 * osw::events::Ring — bounded MPSC FIFO ring used by the event plane.
 *
 * Each tier owns one Ring. Producers (FreeSWITCH dispatch threads —
 * up to 64, per FF-004) push serialized envelope bytes via Push().
 * The single broadcaster thread (one per tier; the rings are NOT
 * shared between tiers) drains via Pop()/WaitAndPopBatch().
 *
 * V1 implementation:
 *   - std::mutex + std::deque + std::condition_variable.
 *   - Simpler, correct, target ≤ 50µs per call achievable on modern
 *     hardware. Lock-free is a future optimization gated on profiling
 *     (W2 contract §"Stop and surface": > 200µs in synthetic test
 *     means lock contention is real and a redesign is required).
 *
 * Overflow semantics: when Push() finds the ring at capacity, it
 * evicts the OLDEST entry (FIFO eviction) and pushes the new entry
 * to the tail. The caller is informed via the bool return AND the
 * `dropped` out-counter is incremented (for the per-tier
 * tier_dropped_total{tier=N} metric).
 *
 * Sequence numbers (`uint64_t seq`) are allocated by the caller via
 * a per-tier atomic — NOT by the ring. The ring stores `(seq, bytes)`
 * pairs so that subscribers' `since_seq` replay can locate a starting
 * point. Sequences are monotonic per tier; the broadcaster's
 * since_seq query is a linear scan that's acceptable because the
 * rings are bounded (default ≤ 16384 entries).
 *
 * Lock order (W2-wide):
 *   1. tier ring mutex (this file's mu_).
 *   2. subscriber send queue mutex.
 *
 * The broadcaster acquires both in this order (pop from ring → push
 * to subscriber queue). The producer acquires only the ring mutex.
 * The gRPC writer thread acquires only the subscriber-queue mutex.
 * NEVER acquire them in the reverse order — Codex review enforces.
 *
 * Threading:
 *   - Push() is MPSC-safe — callable concurrently from any number of
 *     dispatch threads. The mutex serialises enqueue across producers
 *     and the broadcaster.
 *   - Pop() / TryPop() / WaitAndPopBatch() are called only by the
 *     broadcaster thread for THIS tier. They serialise with producers
 *     via the same mutex.
 *   - SnapshotFromSeq() is called by the SubscribeEvents handler at
 *     subscriber registration time to compute the replay window. It
 *     acquires the same mutex; the broadcaster yields briefly.
 *   - Close() flips an atomic flag and broadcasts the condvar so
 *     WaitAndPopBatch() returns promptly. Called on shutdown after
 *     Binder::Stop() has prevented further enqueues.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_RING_H_
#define OSW_EVENTS_RING_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace osw::events {

/// A single entry sitting in the ring: a sequence number + the
/// already-serialized EventEnvelope bytes (held as
/// shared_ptr<const string> so multiple subscriber send queues can
/// share the underlying buffer without copying).
struct RingEntry {
    std::uint64_t seq = 0;
    std::shared_ptr<const std::string> envelope_bytes;
};

class Ring {
  public:
    /// `capacity` is the maximum number of entries the ring holds;
    /// when a Push would exceed it, the oldest entry is evicted.
    /// Must be >= 1; values below cap at 1.
    explicit Ring(std::size_t capacity);

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    Ring(Ring&&) = delete;
    Ring& operator=(Ring&&) = delete;

    /// Push an entry. Returns true if the entry was enqueued WITHOUT
    /// eviction; false if an existing entry was evicted to make room.
    /// In both cases the new entry IS in the ring at the tail.
    ///
    /// On eviction, `*dropped_out` is incremented by 1 (so the caller
    /// can feed the per-tier dropped counter into Health). Pass
    /// nullptr to ignore.
    bool Push(RingEntry entry, std::uint64_t* dropped_out) noexcept;

    /// Non-blocking pop. Returns std::nullopt if the ring is empty
    /// (or Close()'d while empty).
    [[nodiscard]] std::optional<RingEntry> TryPop() noexcept;

    /// Block until at least one entry is available OR Close() is
    /// called OR the timeout expires. On success returns up to
    /// `max_batch` entries (the broadcaster prefers batches to reduce
    /// per-entry overhead). Returns an empty vector if closed or
    /// timed-out empty.
    [[nodiscard]] std::vector<RingEntry> WaitAndPopBatch(
        std::size_t max_batch, std::chrono::milliseconds timeout) noexcept;

    /// Returns a snapshot of all entries currently in the ring whose
    /// `seq > since_seq`. The snapshot is the "replay window" for a
    /// new subscriber connecting with since_seq.
    ///
    /// Returns:
    ///   - found_in_window=true: at least one entry with seq > since_seq
    ///     was present. `entries` is the post-since_seq slice (could
    ///     be empty if since_seq is exactly the current max).
    ///   - found_in_window=false: the ring's minimum seq is greater
    ///     than (since_seq + 1), meaning the requested replay point
    ///     was already evicted. The caller (SubscribeEvents handler)
    ///     should return RESOURCE_EXHAUSTED to the client.
    ///
    /// A NEW subscriber with since_seq == 0 (live-tail only) always
    /// gets found_in_window=true with entries empty — there's nothing
    /// to replay, the broadcaster will deliver subsequent events.
    struct ReplaySnapshot {
        bool found_in_window = false;
        std::vector<RingEntry> entries;
        std::uint64_t current_min_seq = 0;  // ring's lowest seq (0 if empty)
        std::uint64_t current_max_seq = 0;  // ring's highest seq (0 if empty)
    };
    [[nodiscard]] ReplaySnapshot SnapshotFromSeq(std::uint64_t since_seq) const noexcept;

    /// Current size and capacity. For the Health.tierN_ring_fill_pct
    /// metric: fill_pct = 100 * Size() / Capacity().
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

    /// Drain target — close the ring so that:
    ///   - Push() still works (FF-018 unbind hasn't necessarily run
    ///     yet on the producer side; the safe overflow path keeps the
    ///     ring's invariants).
    ///   - WaitAndPopBatch() returns immediately with whatever's
    ///     left in the ring, then keeps returning empty vectors
    ///     promptly (so the broadcaster thread can exit).
    ///
    /// Idempotent. After Close() returns the ring is "draining"; the
    /// broadcaster will pop the rest and then notice `closed_`.
    void Close() noexcept;

    [[nodiscard]] bool IsClosed() const noexcept { return closed_.load(std::memory_order_acquire); }

  private:
    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<RingEntry> q_;  // guarded by mu_
    std::atomic<bool> closed_{false};

    // Codex W2 I-6: highest seq ever pushed into the ring (across the
    // ring's lifetime, NOT just what's currently resident). Used by
    // SnapshotFromSeq to distinguish:
    //   - fresh ring (max_seq_ever_pushed_ == 0)
    //   - ring whose contents were evicted (since_seq < max_seq_ever
    //     but q_ is empty)
    // Both cases land at q_.empty() but only the latter should be
    // reported as "since_seq evicted". Updated under mu_ inside Push().
    std::uint64_t max_seq_ever_pushed_ = 0;
};

}  // namespace osw::events

#endif  // OSW_EVENTS_RING_H_
