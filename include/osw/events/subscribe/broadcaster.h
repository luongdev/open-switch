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

    /// Unregister. Called by the gRPC handler on RPC exit.
    void RemoveSubscriber(const std::string& id);

    [[nodiscard]] std::size_t SubscriberCount() const noexcept;

    [[nodiscard]] std::uint64_t KicksForReason(KickReason r) const noexcept;

    /// Test seam: process a single ring entry against the current
    /// roster without going through the worker-thread loop. Used by
    /// broadcaster_test to assert routing + kick semantics
    /// deterministically.
    void ProcessOneForTesting(Tier tier, RingEntry entry);

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
};

}  // namespace events
}  // namespace osw

#endif  // OSW_EVENTS_SUBSCRIBE_BROADCASTER_H_
