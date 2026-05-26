/*
 * src/events/subscribe/broadcaster.cc
 *
 * Implementation of osw::events::Broadcaster.
 *
 * One worker thread per tier. Each thread:
 *   1. WaitAndPopBatch from its tier's Ring (acquires + releases the
 *      ring mu_ inside Ring; no other lock is held).
 *   2. Take a snapshot of the live subscriber roster (brief roster_mu_
 *      hold; releases before touching subscriber queues).
 *   3. For each entry in the batch, for each subscriber in the snapshot
 *      matching the filter, TryPush onto the subscriber's bounded
 *      SendQueue. On TryPush failure → RequestClose(kQueueFull) +
 *      increment kick counter. Failure does NOT propagate back-pressure
 *      to other subscribers or to the ring.
 *
 * Lock-order discipline (W2-wide):
 *   1. tier ring mu (inside Ring::WaitAndPopBatch)
 *   2. roster_mu_ (brief shared_ptr snapshot copy)
 *   3. subscriber SendQueue mu (inside SendQueue::TryPush)
 *
 * Reverse acquisition NEVER occurs:
 *   - Producers (Binder::HandleEvent) acquire only the ring mu.
 *   - Subscribers' writer threads acquire only the SendQueue mu.
 *   - The broadcaster acquires them in order: ring → roster → queue.
 *
 * The broadcaster does NOT hold the ring mu while pushing to subscriber
 * queues (WaitAndPopBatch returns a vector<RingEntry> and then releases
 * the ring mu before the dispatch loop runs).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/subscribe/broadcaster.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "open_switch/events/v1/events.pb.h"

#include "osw/events/envelope.h"
#include "osw/events/ring.h"
#include "osw/events/subscribe/routing.h"
#include "osw/events/subscribe/send_queue.h"
#include "osw/events/subscribe/subscriber.h"
#include "osw/events/tier.h"
#include "osw/observability/health.h"
#include "osw/observability/log.h"

namespace osw::events {

namespace {

constexpr const char* kSubsystem = "events.broadcaster";

// One batch per worker tick. Bounded so the broadcaster never holds the
// ring mu_ longer than necessary; the rest of the ring is drained on the
// next iteration.
constexpr std::size_t kBatchMax = 64;

// WaitAndPopBatch timeout. Short enough that Stop() observes the
// stop_requested_ flag promptly; long enough to avoid busy-looping.
constexpr auto kBatchTimeout = std::chrono::milliseconds(100);

// The routing-fields scanner lives in `osw/events/subscribe/routing.h`
// so the SubscribeEvents handler can run the same filter on replay
// entries (Codex W2 finding C-1). The broadcaster's hot path simply
// calls ExtractRoutingFields() here.

}  // namespace

Broadcaster::Broadcaster(RingSet* rings, Health* health) noexcept : rings_(rings), health_(health) {
    for (auto& c : kick_counters_)
        c.store(0, std::memory_order_relaxed);
}

Broadcaster::~Broadcaster() noexcept {
    // Defensive: Stop() is idempotent.
    Stop();
}

void Broadcaster::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        osw::log::Warn(kSubsystem, "Broadcaster::Start called twice; ignoring");
        return;
    }
    stop_requested_.store(false, std::memory_order_release);

    workers_[0] = std::thread([this]() { WorkerLoop(Tier::k1Critical); });
    workers_[1] = std::thread([this]() { WorkerLoop(Tier::k2State); });
    workers_[2] = std::thread([this]() { WorkerLoop(Tier::k3Ephemeral); });
    osw::log::Info(kSubsystem, "Broadcaster started (3 worker threads)");
}

void Broadcaster::Stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);

    // Close all rings so WaitAndPopBatch returns promptly.
    if (rings_ != nullptr) {
        rings_->CloseAll();
    }

    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    running_.store(false, std::memory_order_release);

    // Close every subscriber's send queue so the gRPC writer threads
    // exit. The reason is kShutdown — handlers map this to OK status
    // (clean drain), not RESOURCE_EXHAUSTED.
    std::vector<std::shared_ptr<Subscriber>> snapshot;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        snapshot = roster_;
    }
    for (auto& sub : snapshot) {
        if (sub)
            sub->RequestClose(KickReason::kShutdown);
    }

    osw::log::Info(kSubsystem, "Broadcaster stopped");
}

void Broadcaster::AddSubscriber(std::shared_ptr<Subscriber> sub) {
    if (!sub)
        return;
    std::size_t new_count = 0;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        roster_.push_back(std::move(sub));
        new_count = roster_.size();
    }
    if (health_ != nullptr) {
        health_->SetSubscriberCount(static_cast<std::uint32_t>(new_count));
    }
}

void Broadcaster::AddSubscriberAtomic(std::shared_ptr<Subscriber> sub,
                                      const std::function<void(Subscriber&)>& replay_fn) {
    if (!sub)
        return;
    std::size_t new_count = 0;
    {
        // Hold roster_mu_ across the entire replay+add sequence. This
        // forces every per-tier worker thread to block at
        // RosterSnapshot() (their next dispatch step) — any events
        // they have already popped from a ring sit in their local
        // batch vector until we release. When workers proceed, the
        // new subscriber is in the roster and receives every
        // already-popped event. There is no event-loss window between
        // the replay snapshot the caller takes inside `replay_fn` and
        // the subscriber's appearance on the live-tail path.
        //
        // Codex W2 B-1 — the previous AddSubscriber()-after-snapshot
        // sequence allowed live events with seq in (snap_max,
        // post_snap_max] to be dispatched to existing subscribers only,
        // silently dropping them from the new subscriber's stream.
        //
        // Lock order: roster_mu_ → (inside replay_fn) ring mu →
        // SendQueue mu. Workers acquire ring mu and roster mu strictly
        // sequentially (never both), so the reversed (roster→ring)
        // order here cannot deadlock with them.
        std::lock_guard<std::mutex> lk(roster_mu_);
        if (replay_fn) {
            replay_fn(*sub);
        }
        roster_.push_back(std::move(sub));
        new_count = roster_.size();
    }
    if (health_ != nullptr) {
        health_->SetSubscriberCount(static_cast<std::uint32_t>(new_count));
    }
}

void Broadcaster::RemoveSubscriber(const std::string& id) {
    std::size_t new_count = 0;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        roster_.erase(std::remove_if(
                          roster_.begin(),
                          roster_.end(),
                          [&](const std::shared_ptr<Subscriber>& s) { return s && s->Id() == id; }),
                      roster_.end());
        new_count = roster_.size();
    }
    if (health_ != nullptr) {
        health_->SetSubscriberCount(static_cast<std::uint32_t>(new_count));
    }
}

std::size_t Broadcaster::SubscriberCount() const noexcept {
    std::lock_guard<std::mutex> lk(roster_mu_);
    return roster_.size();
}

std::uint64_t Broadcaster::KicksForReason(KickReason r) const noexcept {
    const auto idx = static_cast<std::size_t>(r);
    if (idx >= kick_counters_.size())
        return 0;
    return kick_counters_[idx].load(std::memory_order_relaxed);
}

std::vector<std::shared_ptr<Subscriber>> Broadcaster::RosterSnapshot() const {
    std::lock_guard<std::mutex> lk(roster_mu_);
    return roster_;  // shared_ptr vector copy — refs +1 each
}

void Broadcaster::ProcessOneForTesting(Tier tier, RingEntry entry) {
    auto snapshot = RosterSnapshot();
    Dispatch(tier, entry, snapshot);
}

void Broadcaster::SetPostPopHookForTesting(std::function<void(Tier)> hook) {
    std::lock_guard<std::mutex> lk(post_pop_hook_mu_);
    post_pop_hook_ = std::move(hook);
}

void Broadcaster::Dispatch(Tier tier,
                           const RingEntry& entry,
                           const std::vector<std::shared_ptr<Subscriber>>& roster) {
    if (!entry.envelope_bytes)
        return;

    // Extract routing fields from the serialised envelope so we can run
    // the subscriber filter without a full proto parse per subscriber.
    const RoutingFields rf = ExtractRoutingFields(*entry.envelope_bytes);

    // Effective tier: prefer the tier the broadcaster knows (the tier
    // whose ring this entry came from); fall back to the proto's tier
    // only if the broadcaster's tier is kUnspecified (defensive; should
    // never happen in practice).
    const Tier effective_tier = (tier == Tier::kUnspecified) ? rf.tier : tier;

    for (const auto& sub : roster) {
        if (!sub)
            continue;
        if (sub->IsClosed())
            continue;
        if (!sub->MatchesFilter(effective_tier, rf.event_name, rf.node_id)) {
            continue;
        }
        if (!sub->Queue().TryPush(entry.envelope_bytes)) {
            // SendQueue full — kick the subscriber. The writer thread
            // will surface RESOURCE_EXHAUSTED to the client on next
            // pop. This does NOT propagate back-pressure to the
            // broadcaster or to the ring; we drop the subscriber, not
            // the event.
            sub->RequestClose(KickReason::kQueueFull);
            kick_counters_[static_cast<std::size_t>(KickReason::kQueueFull)].fetch_add(
                1, std::memory_order_relaxed);
            osw::log::Warn(kSubsystem,
                           "subscriber id=%s kicked (queue_full) on tier=%s",
                           sub->Id().c_str(),
                           std::string(ToString(effective_tier)).c_str());
        }
    }

    total_dispatched_.fetch_add(1, std::memory_order_relaxed);
}

void Broadcaster::WorkerLoop(Tier tier) noexcept {
    try {
        if (rings_ == nullptr)
            return;
        Ring* ring = rings_->Get(tier);
        if (ring == nullptr) {
            osw::log::Error(
                kSubsystem, "no ring for tier=%d; worker exiting", static_cast<int>(tier));
            return;
        }
        while (!stop_requested_.load(std::memory_order_acquire)) {
            // Acquires + releases ring mu_ internally. No other lock
            // is held across this call.
            auto batch = ring->WaitAndPopBatch(kBatchMax, kBatchTimeout);
            if (batch.empty()) {
                if (ring->IsClosed()) {
                    // Drain complete (ring closed + nothing left).
                    return;
                }
                continue;  // timeout-empty; loop back
            }
            // Test seam (Codex W2 B-1 race test): a hook fired AFTER
            // popping a non-empty batch but BEFORE we acquire roster_mu_
            // for RosterSnapshot. Production builds leave this empty;
            // no overhead beyond a copy of the function object.
            std::function<void(Tier)> hook;
            {
                std::lock_guard<std::mutex> lk(post_pop_hook_mu_);
                hook = post_pop_hook_;
            }
            if (hook) {
                hook(tier);
            }
            // Brief roster_mu_ hold to snapshot the live subscriber list.
            // We do NOT hold this across the per-subscriber TryPush loop.
            auto roster = RosterSnapshot();
            for (auto& entry : batch) {
                Dispatch(tier, entry, roster);
            }
        }
    } catch (const std::exception& e) {
        osw::log::Error(
            kSubsystem, "WorkerLoop(tier=%d) threw: %s", static_cast<int>(tier), e.what());
    } catch (...) {
        osw::log::Error(
            kSubsystem, "WorkerLoop(tier=%d) threw unknown exception", static_cast<int>(tier));
    }
}

}  // namespace events
}  // namespace osw
