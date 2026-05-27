/*
 * src/observability/health_metrics.cc
 *
 * Implementation of osw::observability::HealthMetrics.
 * See include/osw/observability/health_metrics.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/health_metrics.h"

#include "osw/observability/health.h"
#include "osw/observability/prometheus.h"

namespace osw::observability {

HealthMetrics::HealthMetrics(prometheus::Registry* registry) {
    // --- Subscriber count -----------------------------------------------
    subscriber_gauge_ = registry->AddGauge(
        "osw_events_subscribers", "Current number of active SubscribeEvents gRPC streams", {});

    // --- Tier ring drop counters ----------------------------------------
    // These are rendered as gauges internally but represent a running
    // total (monotonically increasing unless module restarts). Prometheus
    // operators use rate() for per-interval drop rates.
    tier1_drops_gauge_ =
        registry->AddGauge("osw_events_tier_ring_drops_total",
                           "Total events dropped from the tier-1 ring since module load",
                           {{"tier", "1"}});

    tier2_drops_gauge_ =
        registry->AddGauge("osw_events_tier_ring_drops_total",
                           "Total events dropped from the tier-2 ring since module load",
                           {{"tier", "2"}});

    tier3_drops_gauge_ =
        registry->AddGauge("osw_events_tier_ring_drops_total",
                           "Total events dropped from the tier-3 ring since module load",
                           {{"tier", "3"}});

    // --- Audit emit counter ---------------------------------------------
    audit_emit_gauge_ =
        registry->AddGauge("osw_events_audit_emit_total",
                           "Total audit events emitted (osw::audit::Emit calls) since module load",
                           {});
}

void HealthMetrics::Refresh(const osw::Health& health) {
    const auto snap = health.GetSnapshot();

    subscriber_gauge_->Set(static_cast<std::int64_t>(snap.subscriber_count));

    tier1_drops_gauge_->Set(static_cast<std::int64_t>(snap.tier1_dropped_total));
    tier2_drops_gauge_->Set(static_cast<std::int64_t>(snap.tier2_dropped_total));
    tier3_drops_gauge_->Set(static_cast<std::int64_t>(snap.tier3_dropped_total));

    audit_emit_gauge_->Set(static_cast<std::int64_t>(snap.events_emitted_total));
}

}  // namespace osw::observability
