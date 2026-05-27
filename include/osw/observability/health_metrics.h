/*
 * include/osw/observability/health_metrics.h
 *
 * osw::observability::HealthMetrics — Prometheus adapter for osw::Health.
 *
 * Registers a set of Prometheus gauges and counters into a
 * prometheus::Registry that mirror the values in a Health snapshot.
 * The adapter does NOT permanently hold a reference to the Health object
 * — instead, Refresh() is called before each scrape to snapshot the
 * latest values and push them into the registered metrics.
 *
 * Metric inventory (all sourced from Health::Snapshot):
 *
 *   osw_events_subscribers          gauge  — subscriber_count
 *   osw_events_tier_ring_drops_total counter{tier="1|2|3"} — tierN_dropped_total
 *   osw_events_audit_emit_total     counter — events_emitted_total
 *
 * Note on osw_events_tier_ring_drops_total:
 *   Health::tierN_dropped_total is written as a running total by the W2
 *   event ring (each drop increments the value). The Prometheus counter
 *   here shadows it with a Gauge so Refresh() can always set it to the
 *   latest total (we cannot use a monotonic Counter because the Health
 *   field is reset to zero at module restart). Prometheus convention is
 *   to use a counter_total suffix; we use a Gauge under the hood and
 *   render the TYPE as "counter" for correct Prometheus semantics.
 *   Operators who care about rates use `rate()` which works on both.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_HEALTH_METRICS_H_
#define OSW_OBSERVABILITY_HEALTH_METRICS_H_

#include "osw/observability/health.h"
#include "osw/observability/prometheus.h"

namespace osw::observability {

/// Registers Health-derived metrics into a Registry and keeps them
/// up-to-date via Refresh().
class HealthMetrics {
  public:
    /// Register all metrics into `registry`. The registry must outlive
    /// this object.
    explicit HealthMetrics(prometheus::Registry* registry);

    // Non-copyable.
    HealthMetrics(const HealthMetrics&) = delete;
    HealthMetrics& operator=(const HealthMetrics&) = delete;

    /// Snapshot `health` and update all registered metrics. Thread-safe
    /// (Health::GetSnapshot is wait-free; setting the Prometheus gauges
    /// is atomic). Intended to be called from the metrics server's
    /// render callback before Registry::Render().
    void Refresh(const osw::Health& health);

  private:
    // All pointers are non-owning; the registry owns the objects.
    prometheus::Gauge* subscriber_gauge_ = nullptr;

    prometheus::Gauge* tier1_drops_gauge_ = nullptr;
    prometheus::Gauge* tier2_drops_gauge_ = nullptr;
    prometheus::Gauge* tier3_drops_gauge_ = nullptr;

    prometheus::Gauge* audit_emit_gauge_ = nullptr;
};

}  // namespace osw::observability

#endif  // OSW_OBSERVABILITY_HEALTH_METRICS_H_
