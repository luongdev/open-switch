/*
 * src/control/idempotency_cache.cc
 *
 * Implementation of osw::control::IdempotencyCache.
 *
 * Concurrency model (in brief):
 *   - mu_ protects index_, lru_list_, and in_flight_.
 *   - Waiting on an in-flight reservation releases mu_ via condvar wait so
 *     that the executing thread can call Store/Cancel without deadlocking.
 *   - The condvar is notified broadcast-style because multiple waiters can
 *     share the same request_id.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/idempotency_cache.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace osw::control {

IdempotencyCache::IdempotencyCache(std::size_t capacity,
                                   std::chrono::seconds ttl,
                                   std::chrono::steady_clock::duration in_flight_max_wait)
    : capacity_(capacity), ttl_(ttl), in_flight_max_wait_(in_flight_max_wait) {
    index_.reserve(capacity + 1);
}

// ---------------------------------------------------------------------------
// LookupOrReserve
// ---------------------------------------------------------------------------

IdempotencyCache::LookupResult IdempotencyCache::LookupOrReserve(const std::string& request_id) {
    // Empty request_id → caller opted out of deduplication.
    if (request_id.empty()) {
        return LookupResult{State::kMiss, {}};
    }

    std::unique_lock<std::mutex> lk(mu_);

    const auto now = std::chrono::steady_clock::now();

    // --- Check LRU store ---------------------------------------------------
    auto it = index_.find(request_id);
    if (it != index_.end()) {
        LruIterator list_it = it->second;
        const Entry& stored = list_it->second;

        if (stored.expires_at <= now) {
            // Expired entry — evict and fall through to Miss/Reserve path.
            Erase(list_it);
            // If there is also an in-flight reservation left from a previous
            // (now-timed-out) executor, leave it in place so later callers
            // still serialise on it — this is intentional (the timeout path
            // does NOT clear the reservation so the original executor can
            // still call Store and wake everyone up).
        } else {
            // Valid hit — promote to front of LRU list, return cached entry.
            lru_list_.splice(lru_list_.begin(), lru_list_, list_it);
            return LookupResult{State::kHit, stored};
        }
    }

    // --- Check / wait on in-flight -----------------------------------------
    auto inf_it = in_flight_.find(request_id);
    if (inf_it != in_flight_.end()) {
        // Another thread is executing this request_id.  Wait up to
        // in_flight_max_wait_ for it to call Store or Cancel.
        const auto deadline = std::chrono::steady_clock::now() + in_flight_max_wait_;

        const bool signalled = cv_.wait_until(lk, deadline, [&] {
            // Wake condition: either the request is no longer in-flight, or
            // there is now a cached entry for it.
            const bool still_in_flight = (in_flight_.count(request_id) > 0);
            const bool has_entry = (index_.count(request_id) > 0);
            return !still_in_flight || has_entry;
        });

        if (signalled) {
            // Check if a result was stored while we waited.
            auto res_it = index_.find(request_id);
            if (res_it != index_.end()) {
                LruIterator list_it = res_it->second;
                const Entry& stored = list_it->second;
                const auto now2 = std::chrono::steady_clock::now();
                if (stored.expires_at > now2) {
                    lru_list_.splice(lru_list_.begin(), lru_list_, list_it);
                    return LookupResult{State::kHit, stored};
                }
                // Entry expired between Store and our wake — fall through.
                Erase(list_it);
            }
        }
        // Timeout or cancelled without a result: fall through to kMiss.
        // The original executor's in-flight reservation may still exist; we
        // do NOT add another one — the caller will execute independently and
        // call Store (last-write-wins) when done.
        return LookupResult{State::kMiss, {}};
    }

    // --- Miss — reserve the slot -------------------------------------------
    in_flight_.emplace(request_id, false);
    return LookupResult{State::kMiss, {}};
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void IdempotencyCache::Store(const std::string& request_id, Entry entry) {
    if (request_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lk(mu_);

    // Remove any existing entry for this key (last-write-wins; covers the
    // case where a timeout-fallback executor also calls Store).
    auto existing = index_.find(request_id);
    if (existing != index_.end()) {
        Erase(existing->second);
    }

    // Evict if needed (expired + LRU), then insert at front.
    EvictIfNeeded();

    lru_list_.emplace_front(request_id, std::move(entry));
    index_.emplace(request_id, lru_list_.begin());

    // Release in-flight reservation.
    in_flight_.erase(request_id);

    // Wake all waiters for this (and any other) request_id so they can re-
    // check predicates.  Broadcast is correct here: multiple threads can be
    // waiting on different keys, and a spurious wake is harmless.
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

void IdempotencyCache::Cancel(const std::string& request_id) {
    if (request_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lk(mu_);

    in_flight_.erase(request_id);

    // Wake waiters so they can fall through to kMiss and retry independently.
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void IdempotencyCache::EvictIfNeeded() {
    // First pass: evict expired entries (lazy TTL sweep, bounded by list size).
    const auto now = std::chrono::steady_clock::now();
    auto it = lru_list_.end();
    while (it != lru_list_.begin()) {
        --it;
        if (it->second.expires_at <= now) {
            auto next = std::next(it);
            Erase(it);
            it = next;
        }
    }

    // Second pass: LRU eviction until under capacity.
    while (lru_list_.size() >= capacity_) {
        Erase(std::prev(lru_list_.end()));
    }
}

void IdempotencyCache::Erase(LruIterator it) {
    index_.erase(it->first);
    lru_list_.erase(it);
}

}  // namespace osw::control
