/*
 * include/osw/events/subscribe/broadcaster.h
 *
 * osw::events::Broadcaster — fans out events from per-tier rings to
 * per-subscriber send queues.
 *
 * One Broadcaster owns three worker threads, one per tier. Each
 * thread:
 *   1. WaitAndPopBatch on its tier's ring (FF-018 unbind discipline
 *      ensures producers stop before the broadcaster shuts down).
 *   2. For each entry, parse JUST the routing-relevant fields
 *      (tier, event_name, node_id) from the serialised envelope so
 *      the subscriber filter can run without a full proto parse on
 *      the broadcaster's hot path. The bytes themselves go on the
 *      subscriber's send queue as-is (zero-copy via
 *      shared_ptr<const string>).
 *   3. Take a SNAPSHOT of the subscriber roster (under a brief lock
 *      held only for the shared_ptr clone), then iterate WITHOUT
 *      holding the roster mu_. This is the per-subscriber kick path
 *      — pushing into a slow subscriber's queue must not block
 *      pushes to a fast subscriber.
 *   4. For each subscriber matching the filter, TryPush onto the
 *      send queue. On TryPush failure (queue full → RESOURCE_EXHAUSTED):
 *      - subscriber.RequestClose(KickReason::kQueueFull)
 *      - increment broadcaster kick counter
 *      - broadcaster does NOT block on the slow subscriber.
 *
 * Lock order discipline:
 *   tier ring mu (Ring::WaitAndPopBatch) → SendQueue mu (TryPush).
 *   NEVER reversed. The broadcaster releases the ring lock before
 *   touching subscriber queues (WaitAndPopBatch returns, then the
 *   broadcaster operates on its local std::vector<RingEntry>).
 *   The roster mu_ is acquired ONLY to clone the shared_ptr vector;
 *   it's released before TryPush. No mutex is held across both
 *   ring and send-queue access.
 *
 * Threading:
 *   - Start spawns 3 worker threads. Stop joins them. Idempotent.
 *   - AddSubscriber / RemoveSubscriber: serialised by roster_mu_.
 *   - The broadcaster's per-tier counters are atomics.
 *
 * Subscribers' Output:
 *   - Each subscriber's writer thread (the gRPC handler) drains the
 *     SendQueue and calls grpc::ServerWriter::Write on the parsed
 *     envelope. (The W2 contract notes that gRPC's WriteRaw is not
 *     publicly exposed for repeated message streams; the handler
 *     thus re-parses the bytes into an EventEnvelope arena message
 *     before Write. The bytes object stays alive via shared_ptr —
 *     no copy from the ring side.)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_SUBSCRIBE_BROADCASTER_H_
#define OSW_EVENTS_SUBSCRIBE_BROADCASTER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "osw/events/ring.h"
#include "osw/events/subscribe/subscriber.h"
#include "osw/events/tier.h"

namespace osw {
class Health;
namespace events {

class RingSet;

class Broadcaster {
  public:
    /// `rings` and `health` are non-owning. The Module singleton owns
    /// them; the broadcaster borrows.
    Broadcaster(RingSet* rings, Health* health) noexcept;

    ~Broadcaster() noexcept;

    Broadcaster(const Broadcaster&) = delete;
    Broadcaster& operator=(const Broadcaster&) = delete;

    /// Start the 3 broadcaster threads. Idempotent.
    void Start();

    /// Stop the broadcaster: close all rings (so WaitAndPopBatch
    /// returns), join the threads, then close all subscribers'
    /// SendQueues with KickReason::kShutdown. Idempotent.
    void Stop();

    /// Register a new subscriber. The broadcaster holds a shared_ptr
    /// to keep the subscriber alive; the caller (gRPC handler) also
    /// holds one for the RPC's duration.
    void AddSubscriber(std::shared_ptr<Subscriber> sub);

    /// Atomic-against-broadcaster registration (Codex W2 B-1).
    ///
    /// Acquires `roster_mu_`, runs `replay_fn(sub)` (which typically
    /// snapshots a ring + pushes the replay slice into the subscriber's
    /// SendQueue), then appends `sub` to the roster, then releases
    /// `roster_mu_`. The entire sequence is atomic against the
    /// broadcaster's per-tier worker threads, which block at
    /// `RosterSnapshot()` until the roster lock is released. Any
    /// events the workers have already popped from their rings (but
    /// not yet dispatched) are delivered to the new subscriber as
    /// soon as the workers proceed — so no live-tail event in the
    /// window between snapshot and add is lost.
    ///
    /// Lock order: `roster_mu_` is acquired first; per-tier ring mus
    /// are acquired transiently inside `replay_fn` via
    /// `Ring::SnapshotFromSeq`. Workers acquire ring mu → release →
    /// roster mu (never both at once), so the reversed-acquisition
    /// path (roster → ring) in `replay_fn` cannot deadlock with them.
    ///
    /// The replay closure SHOULD complete promptly — workers stall
    /// while the lock is held. For typical replay-from-ring sizes
    /// (≤ ring capacity entries × O(µs) TryPush per entry) the stall
    /// is sub-millisecond.
    void AddSubscriberAtomic(std::shared_ptr<Subscriber> sub,
                             const std::function<void(Subscriber&)>& replay_fn);

    /// Unregister. Called by the gRPC handler on RPC exit.
    void RemoveSubscriber(const std::string& id);

    [[nodiscard]] std::size_t SubscriberCount() const noexcept;

    [[nodiscard]] std::uint64_t KicksForReason(KickReason r) const noexcept;

    /// Test seam: process a single ring entry against the current
    /// roster without going through the worker-thread loop. Used by
    /// broadcaster_test to assert routing + kick semantics
    /// deterministically.
    void ProcessOneForTesting(Tier tier, RingEntry entry);

    /// Test seam: install a function the per-tier worker invokes
    /// AFTER popping a non-empty batch from its ring but BEFORE it
    /// acquires `roster_mu_` for RosterSnapshot. Used by
    /// subscribe_replay_test's race fixture to deterministically
    /// expose the replay→live-tail gap window (Codex W2 B-1). The
    /// hook is called with the tier; passing an empty function (or
    /// nullptr-equivalent default-constructed std::function) disables
    /// it. The store/load uses acquire/release on a shared_ptr load
    /// (atomic with respect to other workers) so reconfiguration
    /// during a test run is safe.
    void SetPostPopHookForTesting(std::function<void(Tier)> hook);

  private:
    void WorkerLoop(Tier tier) noexcept;

    /// Take a snapshot of the current roster. Brief roster_mu_ hold.
    std::vector<std::shared_ptr<Subscriber>> RosterSnapshot() const;

    /// Dispatch a single entry to all matching subscribers in the
    /// snapshot. Performs the lock-order-safe per-subscriber push.
    void Dispatch(Tier tier,
                  const RingEntry& entry,
                  const std::vector<std::shared_ptr<Subscriber>>& roster);

    RingSet* rings_;
    Health* health_;
    std::array<std::thread, 3> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Roster of in-flight subscribers. We use shared_ptr so a slow
    // subscriber being kicked doesn't tear out from under a
    // broadcaster thread mid-dispatch.
    mutable std::mutex roster_mu_;
    std::vector<std::shared_ptr<Subscriber>> roster_;

    // Kick counters per reason. Sized to KickReason max + 1.
    static constexpr std::size_t kNumKickReasons = 5;
    std::array<std::atomic<std::uint64_t>, kNumKickReasons> kick_counters_;

    // Health bridge — see broadcaster.cc.
    std::atomic<std::uint64_t> total_dispatched_{0};

    // Test-only seam (default-constructed → disabled). Guarded by
    // `post_pop_hook_mu_` for installation; the worker loads a copy
    // under that mu (only on batches that aren't empty; the empty-
    // batch fast path is unaffected).
    mutable std::mutex post_pop_hook_mu_;
    std::function<void(Tier)> post_pop_hook_;
};

}  // namespace events
}  // namespace osw

#endif  // OSW_EVENTS_SUBSCRIBE_BROADCASTER_H_
