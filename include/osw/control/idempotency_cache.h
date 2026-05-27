/*
 * include/osw/control/idempotency_cache.h
 *
 * IdempotencyCache — per-process LRU cache that deduplicates gRPC write
 * RPCs (Originate, Bridge, Execute) by request_id.
 *
 * Design:
 *   - LRU eviction at `capacity` via std::list<KeyEntryPair> +
 *     std::unordered_map<string, ListIterator>.
 *   - TTL checked lazily on every LookupOrReserve (no background sweeper;
 *     capacity=1500 makes a sweeper unnecessary).
 *   - In-flight reservation: a secondary unordered_map tracks request_ids
 *     currently being executed (kMiss was returned, Store not yet called).
 *     Concurrent calls for the same request_id block on a single condvar
 *     (with a per-key bit in a predicate map) up to `in_flight_max_wait`.
 *     The cache mutex is NOT held while waiting; a narrow re-lock checks
 *     the predicate and re-releases immediately.
 *   - Empty request_id → LookupOrReserve returns kMiss without touching
 *     any cache state.
 *
 * Thread-safety: all public methods are thread-safe.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_IDEMPOTENCY_CACHE_H_
#define OSW_CONTROL_IDEMPOTENCY_CACHE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <grpcpp/grpcpp.h>

namespace osw::control {

/// LRU idempotency cache for control-plane write RPCs.
class IdempotencyCache {
  public:
    /// A stored result for one request_id.
    struct Entry {
        grpc::Status status;
        std::string serialized_response;  // proto-serialized response bytes
        std::chrono::steady_clock::time_point expires_at;
    };

    enum class State {
        kMiss,      // no entry; caller must execute and call Store
        kHit,       // cached result returned in LookupResult::entry
        kInFlight,  // another thread is executing; waiter timed out → kMiss
    };

    struct LookupResult {
        State state = State::kMiss;
        Entry entry;  // populated only when state == kHit
    };

    /// Construct with capacity, TTL, and in-flight wait ceiling.
    ///   capacity          — max number of stored entries before LRU eviction.
    ///   ttl               — how long a stored entry is considered valid.
    ///   in_flight_max_wait — how long a waiter blocks before falling back
    ///                        to kMiss (the original executor still owns the
    ///                        slot; this avoids permanent stalls).
    IdempotencyCache(std::size_t capacity,
                     std::chrono::seconds ttl,
                     std::chrono::steady_clock::duration in_flight_max_wait);

    /// Returns the configured TTL.  Handlers use this to compute
    /// Entry::expires_at without depending on the internal field directly.
    [[nodiscard]] std::chrono::seconds Ttl() const noexcept { return ttl_; }

    // Non-copyable, non-movable (owns mutex + condvar).
    IdempotencyCache(const IdempotencyCache&) = delete;
    IdempotencyCache& operator=(const IdempotencyCache&) = delete;
    IdempotencyCache(IdempotencyCache&&) = delete;
    IdempotencyCache& operator=(IdempotencyCache&&) = delete;

    ~IdempotencyCache() = default;

    /// Look up request_id.
    ///
    ///   kHit    → cached Entry returned; caller replays it immediately.
    ///   kMiss   → no entry (or expired); caller must execute and call
    ///             Store.  A reservation is written atomically so that
    ///             concurrent callers with the same request_id will block
    ///             (or fall through on timeout) rather than double-execute.
    ///   kMiss   (after wait timeout) → the in-flight reservation for this
    ///             request_id still exists (owned by the original executor);
    ///             this waiter gives up and executes itself, then calls
    ///             Store with last-write-wins semantics.
    ///
    /// Empty request_id → always returns kMiss without any state change.
    LookupResult LookupOrReserve(const std::string& request_id);

    /// Persist the result of an execution.  Wakes any threads waiting on
    /// the in-flight reservation for request_id.  Overwrites any existing
    /// entry (last-write-wins; safe for the timeout-fallback path above).
    /// No-op if request_id is empty.
    void Store(const std::string& request_id, Entry entry);

    /// Release the in-flight reservation without storing a result (used in
    /// error/exception paths where no definitive outcome was produced).
    /// Wakes waiters, which will observe kMiss and retry independently.
    /// No-op if request_id is empty or not reserved.
    void Cancel(const std::string& request_id);

  private:
    // -----------------------------------------------------------------------
    // LRU store
    // -----------------------------------------------------------------------

    // Each node in the LRU list.
    using KeyEntryPair = std::pair<std::string, Entry>;
    using LruList = std::list<KeyEntryPair>;
    using LruIterator = LruList::iterator;

    // Map from request_id to its position in lru_list_.
    std::unordered_map<std::string, LruIterator> index_;

    // LRU list: front = most recently used, back = least recently used.
    LruList lru_list_;

    // -----------------------------------------------------------------------
    // In-flight reservation set
    // -----------------------------------------------------------------------

    // request_ids currently being executed (Store/Cancel not yet called).
    std::unordered_map<std::string, bool> in_flight_;  // value unused; key presence = reserved

    // -----------------------------------------------------------------------
    // Synchronisation
    // -----------------------------------------------------------------------

    std::mutex mu_;
    std::condition_variable cv_;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    const std::size_t capacity_;
    const std::chrono::seconds ttl_;
    const std::chrono::steady_clock::duration in_flight_max_wait_;

    // -----------------------------------------------------------------------
    // Internal helpers (called with mu_ held unless noted)
    // -----------------------------------------------------------------------

    /// Evict expired entries first; then, if still at capacity, evict the
    /// least-recently-used entry.  Called before inserting a new entry.
    void EvictIfNeeded();

    /// Remove entry from both lru_list_ and index_.  `it` must be a valid
    /// iterator into lru_list_.
    void Erase(LruIterator it);
};

}  // namespace osw::control

#endif  // OSW_CONTROL_IDEMPOTENCY_CACHE_H_
