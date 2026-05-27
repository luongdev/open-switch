/*
 * include/osw/observability/prometheus.h
 *
 * osw::observability::prometheus — Prometheus text exposition format
 * primitives (Counter, Histogram) and a Registry that collects all
 * registered metrics and renders them to the wire format.
 *
 * Design goals:
 *   - Zero dependencies beyond the C++ standard library.
 *   - Thread-safe: all public mutators use std::atomic; Render() takes
 *     a snapshot and is safe to call from the HTTP scrape thread while
 *     other threads update counters.
 *   - Simple flat API: Counter/Histogram are value types allocated by
 *     the registry and exposed via raw pointers. Lifetime: all counters
 *     must outlive the last Render() call (module-lifetime in practice).
 *
 * Prometheus text format spec (simplified V1 subset):
 *
 *   # HELP <name> <description>
 *   # TYPE <name> counter|gauge|histogram
 *   <name>{label_key="label_val",...} <value>
 *
 * For histograms the rendered block is:
 *   <name>_bucket{le="0.005",<extra_labels>} <count>
 *   ...
 *   <name>_bucket{le="+Inf",<extra_labels>} <count>
 *   <name>_sum{<extra_labels>} <total_seconds>
 *   <name>_count{<extra_labels>} <observations>
 *
 * Default latency histogram buckets (seconds):
 *   0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10
 *
 * Logger subsystem: "metrics.prometheus"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_PROMETHEUS_H_
#define OSW_OBSERVABILITY_PROMETHEUS_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osw::observability::prometheus {

// ---------------------------------------------------------------------------
// Label helpers
// ---------------------------------------------------------------------------

/// A single label key=value pair.
struct Label {
    std::string key;
    std::string value;
};

using Labels = std::vector<Label>;

/// Render a label set into the `{k="v",...}` suffix.
/// Returns "" when `labels` is empty.
std::string RenderLabels(const Labels& labels);

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

/// A monotonically increasing uint64 counter. Thread-safe.
class Counter {
  public:
    Counter(std::string name, std::string help, Labels labels)
        : name_(std::move(name)), help_(std::move(help)), labels_(std::move(labels)), value_(0) {}

    /// Increment by `delta` (default 1). Thread-safe.
    void Inc(std::uint64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    /// Snapshot the current value. Thread-safe (relaxed load).
    [[nodiscard]] std::uint64_t Get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }
    [[nodiscard]] const Labels& labels() const noexcept { return labels_; }

    /// Render HELP + TYPE + value lines into `out`.
    void Render(std::string& out) const;

  private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<std::uint64_t> value_;
};

// ---------------------------------------------------------------------------
// Gauge
// ---------------------------------------------------------------------------

/// A signed gauge metric (can go up and down). Thread-safe.
class Gauge {
  public:
    Gauge(std::string name, std::string help, Labels labels)
        : name_(std::move(name)), help_(std::move(help)), labels_(std::move(labels)), value_(0) {}

    /// Set the gauge to `v`. Thread-safe.
    void Set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }

    /// Increment by `delta`. Thread-safe.
    void Inc(std::int64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    /// Decrement by `delta`. Thread-safe.
    void Dec(std::int64_t delta = 1) noexcept {
        value_.fetch_sub(delta, std::memory_order_relaxed);
    }

    /// Snapshot the current value.
    [[nodiscard]] std::int64_t Get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }
    [[nodiscard]] const Labels& labels() const noexcept { return labels_; }

    /// Render HELP + TYPE + value lines into `out`.
    void Render(std::string& out) const;

  private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<std::int64_t> value_;
};

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

/// Default latency bucket boundaries (seconds), per the W4 spec.
static constexpr std::array<double, 11> kDefaultLatencyBuckets = {
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};

/// A Prometheus histogram. Thread-safe (bucket counts and sum are
/// individually atomic; cross-bucket skew is acceptable for a scrape
/// endpoint). Labels are fixed at construction time.
class Histogram {
  public:
    /// `bounds` defines the upper-inclusive bucket boundaries. An
    /// implicit +Inf bucket is always appended. Use kDefaultLatencyBuckets
    /// for gRPC latency.
    Histogram(std::string name, std::string help, Labels labels, const std::vector<double>& bounds);

    /// Record one observation of `value` (in the unit of the histogram).
    void Observe(double value) noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }
    [[nodiscard]] const Labels& labels() const noexcept { return labels_; }
    [[nodiscard]] const std::vector<double>& bounds() const noexcept { return bounds_; }

    /// Snapshot bucket counts (size == bounds().size() + 1 for +Inf).
    [[nodiscard]] std::vector<std::uint64_t> GetBucketCounts() const;

    /// Snapshot sum of all observed values.
    [[nodiscard]] double GetSum() const noexcept;

    /// Snapshot total number of observations.
    [[nodiscard]] std::uint64_t GetCount() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    /// Render HELP + TYPE + bucket/sum/count lines into `out`.
    void Render(std::string& out) const;

  private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::vector<double> bounds_;  // sorted upper bounds (excluding +Inf)

    // bucket_counts_[i] is the cumulative count of observations <=
    // bounds_[i]. The implicit +Inf bucket is bucket_counts_.back().
    std::vector<std::atomic<std::uint64_t>> bucket_counts_;

    // sum_ is stored as a uint64 of nanoseconds to avoid the need for
    // a compare-exchange loop on the double — nanosecond precision is
    // more than enough for gRPC latency histograms (which are in seconds
    // with at most 10s as the largest bucket). We convert to seconds on
    // render.
    std::atomic<std::uint64_t> sum_ns_{0};
    std::atomic<std::uint64_t> count_{0};
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

/// Central registry that owns all metrics and renders the full page.
///
/// Usage pattern (module-lifetime singleton):
///   auto& reg = Registry::Global();
///   auto* calls = reg.AddCounter("osw_rpc_calls_total", "RPC call count",
///                                {{"rpc","Health"},{"code","OK"}});
///   calls->Inc();
///   std::string page = reg.Render();
///
/// Thread-safe: AddCounter/AddGauge/AddHistogram are protected by a mutex
/// (called only during init). Render() is safe from any thread; it takes
/// a snapshot of all registered metrics.
class Registry {
  public:
    Registry() = default;

    // Non-copyable, non-movable (module-lifetime singleton).
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /// Add a counter. Registry owns the counter. Returns a raw pointer
    /// (non-owning) for the caller to record observations. The pointer
    /// is stable for the lifetime of the registry.
    Counter* AddCounter(std::string name, std::string help, Labels labels = {});

    /// Add a gauge.
    Gauge* AddGauge(std::string name, std::string help, Labels labels = {});

    /// Add a histogram with custom bounds.
    Histogram* AddHistogram(std::string name,
                            std::string help,
                            Labels labels,
                            const std::vector<double>& bounds);

    /// Add a histogram with the default latency buckets.
    Histogram* AddLatencyHistogram(std::string name, std::string help, Labels labels = {});

    /// Render all registered metrics to the Prometheus text format.
    /// Thread-safe; metrics are read with relaxed atomics (scrape-level
    /// consistency is fine for Prometheus).
    [[nodiscard]] std::string Render() const;

    /// Clear all registered metrics. Intended for testing only.
    void ClearForTesting();

  private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<Counter>> counters_;
    std::vector<std::unique_ptr<Gauge>> gauges_;
    std::vector<std::unique_ptr<Histogram>> histograms_;
};

}  // namespace osw::observability::prometheus

#endif  // OSW_OBSERVABILITY_PROMETHEUS_H_
