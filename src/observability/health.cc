/*
 * src/observability/health.cc
 *
 * Implementation of osw::Health. See include/osw/observability/health.h.
 *
 * Load-time only contract: SetVersions must be called exactly once
 * during Module::Load before any reader observes this instance. The
 * happens-before from the worker-thread spawn establishes the
 * visibility for the gRPC service thread. Subsequent reads via
 * Snapshot race against the never-modified strings — safe.
 *
 * The counter loads in GetSnapshot use std::memory_order_acquire so
 * each individual counter is consistent with its most recent store,
 * but cross-counter consistency is not enforced (and not required for
 * a health endpoint).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/health.h"

#include <utility>

namespace osw {

void Health::SetVersions(std::string module_version, std::string freeswitch_version) {
    module_version_ = std::move(module_version);
    freeswitch_version_ = std::move(freeswitch_version);
}

Health::Snapshot Health::GetSnapshot() const {
    Snapshot s;
    s.status = status_.load(std::memory_order_acquire);
    s.module_version = module_version_;
    s.freeswitch_version = freeswitch_version_;
    s.active_channels = active_channels_.load(std::memory_order_acquire);
    s.active_media_bugs = active_media_bugs_.load(std::memory_order_acquire);
    s.events_emitted_total = events_emitted_total_.load(std::memory_order_acquire);
    s.subscriber_count = subscriber_count_.load(std::memory_order_acquire);
    s.tier1_ring_fill_pct = tier1_ring_fill_pct_.load(std::memory_order_acquire);
    s.tier2_ring_fill_pct = tier2_ring_fill_pct_.load(std::memory_order_acquire);
    s.tier3_ring_fill_pct = tier3_ring_fill_pct_.load(std::memory_order_acquire);
    s.tier1_dropped_total = tier1_dropped_total_.load(std::memory_order_acquire);
    s.tier2_dropped_total = tier2_dropped_total_.load(std::memory_order_acquire);
    s.tier3_dropped_total = tier3_dropped_total_.load(std::memory_order_acquire);
    return s;
}

}  // namespace osw
