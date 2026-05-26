/*
 * src/events/ring_set.cc
 *
 * Implementation of osw::events::RingSet — the bundle-of-three-Rings
 * type declared alongside Binder in osw/events/binder.h.
 *
 * Lives in osw_events (FS-agnostic) NOT osw_events_fs, because RingSet
 * holds three Ring values and nothing else — no <switch.h>, no FS APIs.
 * The Broadcaster (osw_events) references RingSet::Get() and
 * RingSet::CloseAll(), so these definitions MUST be in the same
 * library or the FS-agnostic test executables fail to link.
 *
 * The Binder methods (which DO need <switch.h>) stay in binder.cc
 * under osw_events_fs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <chrono>
#include <cstddef>
#include <mutex>

#include "osw/events/binder.h"
#include "osw/events/ring.h"
#include "osw/events/tier.h"

namespace osw::events {

RingSet::RingSet(std::size_t cap1, std::size_t cap2, std::size_t cap3)
    : tier1_(cap1), tier2_(cap2), tier3_(cap3) {
    // Gemini W2.5 I-4: wire each ring's drain-notifier to a single
    // bound callback that signals our drain condvar. The Ring invokes
    // the notifier with its own `mu_` held; the callback only does
    // `notify_all` on `drain_cv_` which does NOT acquire any other
    // lock, so the ring-mu → roster-mu → SendQueue-mu lock order
    // remains unviolated.
    auto on_drain = [this]() noexcept { NotifyDrainTransition(); };
    tier1_.SetDrainNotifier(on_drain);
    tier2_.SetDrainNotifier(on_drain);
    tier3_.SetDrainNotifier(on_drain);
}

Ring* RingSet::Get(Tier t) noexcept {
    switch (t) {
        case Tier::k1Critical:
            return &tier1_;
        case Tier::k2State:
            return &tier2_;
        case Tier::k3Ephemeral:
            return &tier3_;
        case Tier::kUnspecified:
        default:
            return nullptr;
    }
}

void RingSet::CloseAll() noexcept {
    tier1_.Close();
    tier2_.Close();
    tier3_.Close();
    // Also kick any pending shutdown waiters — Close() means producers
    // are gone; once the broadcaster drains the residual entries the
    // wait predicate will trip. Without this, a waiter that called
    // WaitUntilAllEmpty before CloseAll could miss the very last drain
    // notification if it happened to fall between two condvar checks.
    NotifyDrainTransition();
}

bool RingSet::AllEmpty() const noexcept {
    return tier1_.Size() == 0 && tier2_.Size() == 0 && tier3_.Size() == 0;
}

bool RingSet::WaitUntilAllEmpty(std::chrono::steady_clock::time_point deadline) noexcept {
    // Lock-order discipline (see binder.h on the drain_mu_ field):
    // `AllEmpty()` calls `Ring::Size()` which acquires each ring's
    // mu_. The notifier path runs ring_mu → drain_mu; if we held
    // drain_mu while calling AllEmpty (which acquires ring_mu) we'd
    // reverse it and deadlock.
    //
    // Instead: snapshot drain_generation_ under drain_mu, check
    // AllEmpty OUT of drain_mu, then re-enter wait_until with the
    // predicate `generation changed` to sleep until the next drain
    // transition or the deadline.
    while (true) {
        if (AllEmpty()) {
            return true;
        }
        std::unique_lock<std::mutex> lk(drain_mu_);
        const auto seen_generation = drain_generation_;
        const bool woke = drain_cv_.wait_until(
            lk, deadline, [&]() noexcept { return drain_generation_ != seen_generation; });
        lk.unlock();
        if (!woke) {
            // wait_until timed out — predicate still false. Honour the
            // deadline strictly; one final AllEmpty() peek in case the
            // last drain notification arrived after we entered the
            // wait but before the timer fired (race window — harmless
            // to double-check).
            return AllEmpty();
        }
        // Predicate satisfied → loop and re-check AllEmpty() outside
        // drain_mu_.
    }
}

void RingSet::NotifyDrainTransition() noexcept {
    // The Ring calls this with its own mu_ held. We acquire drain_mu_
    // briefly (no other lock involved) — preserved lock order.
    std::lock_guard<std::mutex> lk(drain_mu_);
    ++drain_generation_;
    drain_cv_.notify_all();
}

}  // namespace osw::events
