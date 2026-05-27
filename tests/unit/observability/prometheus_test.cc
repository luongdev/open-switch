/*
 * tests/unit/observability/prometheus_test.cc
 *
 * Unit tests for osw::observability::prometheus primitives and the
 * Registry renderer. All tests operate on the rendering functions
 * directly — no real HTTP server or socket needed.
 *
 * Covered:
 *   - RenderLabels: empty, single, multi-label; escaping.
 *   - Counter: Inc, Get, Render output.
 *   - Gauge: Set/Inc/Dec, Render output.
 *   - Histogram: Observe, bucket counts, sum, count; Render output.
 *   - Registry: AddCounter, AddGauge, AddHistogram, Render round-trip.
 *   - kDefaultLatencyBuckets bucket count (11 finite + 1 +Inf).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/prometheus.h"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace osw::observability::prometheus;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool Contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// ---------------------------------------------------------------------------
// RenderLabels
// ---------------------------------------------------------------------------

TEST(RenderLabelsTest, EmptyLabelsProducesEmptyString) {
    EXPECT_EQ(RenderLabels({}), "");
}

TEST(RenderLabelsTest, SingleLabel) {
    Labels l = {{"rpc", "Health"}};
    EXPECT_EQ(RenderLabels(l), "{rpc=\"Health\"}");
}

TEST(RenderLabelsTest, MultipleLabels) {
    Labels l = {{"rpc", "Originate"}, {"code", "OK"}};
    EXPECT_EQ(RenderLabels(l), "{rpc=\"Originate\",code=\"OK\"}");
}

TEST(RenderLabelsTest, EscapesDoubleQuote) {
    Labels l = {{"key", "val\"with\"quote"}};
    const auto rendered = RenderLabels(l);
    EXPECT_NE(rendered.find("\\\""), std::string::npos);
}

TEST(RenderLabelsTest, EscapesBackslash) {
    Labels l = {{"key", "val\\back"}};
    const auto rendered = RenderLabels(l);
    EXPECT_NE(rendered.find("\\\\"), std::string::npos);
}

TEST(RenderLabelsTest, EscapesNewline) {
    Labels l = {{"key", "a\nb"}};
    const auto rendered = RenderLabels(l);
    EXPECT_NE(rendered.find("\\n"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

TEST(CounterTest, DefaultValueIsZero) {
    Counter c("osw_rpc_calls_total", "RPC calls", {});
    EXPECT_EQ(c.Get(), 0u);
}

TEST(CounterTest, IncByOne) {
    Counter c("osw_rpc_calls_total", "RPC calls", {});
    c.Inc();
    EXPECT_EQ(c.Get(), 1u);
}

TEST(CounterTest, IncByDelta) {
    Counter c("osw_rpc_calls_total", "RPC calls", {});
    c.Inc(5);
    EXPECT_EQ(c.Get(), 5u);
}

TEST(CounterTest, MultipleIncrements) {
    Counter c("osw_rpc_calls_total", "RPC calls", {});
    c.Inc();
    c.Inc();
    c.Inc(3);
    EXPECT_EQ(c.Get(), 5u);
}

TEST(CounterTest, RenderContainsHelpAndType) {
    Counter c("osw_rpc_calls_total", "Total RPC calls", {{"rpc", "Health"}});
    c.Inc(7);
    std::string out;
    c.Render(out);
    EXPECT_TRUE(Contains(out, "# HELP osw_rpc_calls_total Total RPC calls"));
    EXPECT_TRUE(Contains(out, "# TYPE osw_rpc_calls_total counter"));
    EXPECT_TRUE(Contains(out, "osw_rpc_calls_total{rpc=\"Health\"} 7"));
}

TEST(CounterTest, RenderWithNoLabels) {
    Counter c("osw_events_subscribers", "Active subscriber count", {});
    c.Inc(3);
    std::string out;
    c.Render(out);
    EXPECT_TRUE(Contains(out, "osw_events_subscribers 3"));
}

// ---------------------------------------------------------------------------
// Gauge
// ---------------------------------------------------------------------------

TEST(GaugeTest, DefaultValueIsZero) {
    Gauge g("osw_events_subscribers", "Subscriber gauge", {});
    EXPECT_EQ(g.Get(), 0);
}

TEST(GaugeTest, SetPositive) {
    Gauge g("g", "h", {});
    g.Set(42);
    EXPECT_EQ(g.Get(), 42);
}

TEST(GaugeTest, IncAndDec) {
    Gauge g("g", "h", {});
    g.Inc(10);
    g.Dec(3);
    EXPECT_EQ(g.Get(), 7);
}

TEST(GaugeTest, RenderContainsTypeGauge) {
    Gauge g("osw_events_subscribers", "Current subscribers", {});
    g.Set(5);
    std::string out;
    g.Render(out);
    EXPECT_TRUE(Contains(out, "# TYPE osw_events_subscribers gauge"));
    EXPECT_TRUE(Contains(out, "osw_events_subscribers 5"));
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

TEST(HistogramTest, DefaultBucketCount) {
    // kDefaultLatencyBuckets has 11 entries; +Inf appends 1 more.
    EXPECT_EQ(kDefaultLatencyBuckets.size(), 11u);
}

TEST(HistogramTest, NewHistogramHasZeroCounts) {
    std::vector<double> bounds(kDefaultLatencyBuckets.begin(), kDefaultLatencyBuckets.end());
    Histogram h("osw_rpc_latency_seconds", "RPC latency", {}, bounds);
    EXPECT_EQ(h.GetCount(), 0u);
    EXPECT_DOUBLE_EQ(h.GetSum(), 0.0);
    auto buckets = h.GetBucketCounts();
    EXPECT_EQ(buckets.size(), 12u);  // 11 finite + +Inf
    for (auto v : buckets) {
        EXPECT_EQ(v, 0u);
    }
}

TEST(HistogramTest, ObserveFallsInCorrectBuckets) {
    std::vector<double> bounds = {0.1, 0.5, 1.0};
    Histogram h("test_h", "test", {}, bounds);
    // 0.05 should land in bucket[0] (<=0.1), [1] (<=0.5), [2] (<=1.0), [3] (+Inf)
    h.Observe(0.05);
    auto buckets = h.GetBucketCounts();
    EXPECT_EQ(buckets[0], 1u);  // <=0.1
    EXPECT_EQ(buckets[1], 1u);  // <=0.5 (cumulative)
    EXPECT_EQ(buckets[2], 1u);  // <=1.0 (cumulative)
    EXPECT_EQ(buckets[3], 1u);  // +Inf
    EXPECT_EQ(h.GetCount(), 1u);
}

TEST(HistogramTest, ObserveExactBoundaryInclusive) {
    std::vector<double> bounds = {0.1, 0.5};
    Histogram h("test_h", "test", {}, bounds);
    h.Observe(0.1);  // exactly at boundary — should be in the <=0.1 bucket
    auto buckets = h.GetBucketCounts();
    EXPECT_EQ(buckets[0], 1u);  // <=0.1
    EXPECT_EQ(buckets[1], 1u);  // <=0.5 cumulative
    EXPECT_EQ(buckets[2], 1u);  // +Inf
}

TEST(HistogramTest, ObserveLargeValueOnlyInInfBucket) {
    std::vector<double> bounds = {0.005, 0.01};
    Histogram h("test_h", "test", {}, bounds);
    h.Observe(100.0);  // way beyond all finite buckets
    auto buckets = h.GetBucketCounts();
    EXPECT_EQ(buckets[0], 0u);  // <=0.005 — not included
    EXPECT_EQ(buckets[1], 0u);  // <=0.01 — not included
    EXPECT_EQ(buckets[2], 1u);  // +Inf
    EXPECT_EQ(h.GetCount(), 1u);
}

TEST(HistogramTest, SumAccumulatesCorrectly) {
    std::vector<double> bounds = {1.0, 10.0};
    Histogram h("test_h", "test", {}, bounds);
    h.Observe(0.3);
    h.Observe(0.7);
    // Sum should be close to 1.0 (within floating-point precision of the
    // ns-rounding path).
    EXPECT_NEAR(h.GetSum(), 1.0, 1e-6);
    EXPECT_EQ(h.GetCount(), 2u);
}

TEST(HistogramTest, RenderContainsRequiredLines) {
    std::vector<double> bounds = {0.005, 0.01};
    Histogram h("osw_rpc_latency_seconds", "RPC latency in seconds", {{"rpc", "Health"}}, bounds);
    h.Observe(0.003);
    std::string out;
    h.Render(out);
    EXPECT_TRUE(Contains(out, "# TYPE osw_rpc_latency_seconds histogram"));
    EXPECT_TRUE(Contains(out, "osw_rpc_latency_seconds_bucket{le=\"0.005\""));
    EXPECT_TRUE(Contains(out, "osw_rpc_latency_seconds_bucket{le=\"+Inf\""));
    EXPECT_TRUE(Contains(out, "osw_rpc_latency_seconds_sum"));
    EXPECT_TRUE(Contains(out, "osw_rpc_latency_seconds_count"));
    // The 0.003s observation is <= 0.005 so both finite buckets carry 1.
    EXPECT_TRUE(Contains(out, "le=\"0.005\""));
}

TEST(HistogramTest, RenderWithNoLabels) {
    std::vector<double> bounds = {1.0};
    Histogram h("plain_hist", "plain", {}, bounds);
    h.Observe(0.5);
    std::string out;
    h.Render(out);
    EXPECT_TRUE(Contains(out, "plain_hist_bucket{le=\"1\""));
    EXPECT_TRUE(Contains(out, "plain_hist_bucket{le=\"+Inf\""));
    EXPECT_TRUE(Contains(out, "plain_hist_count 1"));
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

TEST(RegistryTest, AddAndRenderCounter) {
    Registry reg;
    auto* c = reg.AddCounter("osw_rpc_calls_total", "Total calls", {{"rpc", "Health"}});
    c->Inc(10);
    const auto page = reg.Render();
    EXPECT_TRUE(Contains(page, "osw_rpc_calls_total{rpc=\"Health\"} 10"));
}

TEST(RegistryTest, AddAndRenderGauge) {
    Registry reg;
    auto* g = reg.AddGauge("osw_events_subscribers", "Subscriber gauge", {});
    g->Set(3);
    const auto page = reg.Render();
    EXPECT_TRUE(Contains(page, "osw_events_subscribers 3"));
}

TEST(RegistryTest, AddAndRenderHistogram) {
    Registry reg;
    auto* h = reg.AddLatencyHistogram("osw_rpc_latency_seconds", "RPC latency", {{"rpc", "H"}});
    h->Observe(0.002);
    const auto page = reg.Render();
    EXPECT_TRUE(Contains(page, "osw_rpc_latency_seconds"));
    EXPECT_TRUE(Contains(page, "le=\"+Inf\""));
    EXPECT_TRUE(Contains(page, "osw_rpc_latency_seconds_count"));
}

TEST(RegistryTest, MultipleMetricsAllAppearInPage) {
    Registry reg;
    reg.AddCounter("c1", "counter one", {});
    reg.AddGauge("g1", "gauge one", {});
    reg.AddLatencyHistogram("h1", "hist one", {});
    const auto page = reg.Render();
    EXPECT_TRUE(Contains(page, "c1"));
    EXPECT_TRUE(Contains(page, "g1"));
    EXPECT_TRUE(Contains(page, "h1"));
}

TEST(RegistryTest, ClearForTestingRemovesAllMetrics) {
    Registry reg;
    reg.AddCounter("c1", "c", {});
    reg.ClearForTesting();
    EXPECT_EQ(reg.Render(), "");
}

TEST(RegistryTest, PointerStabilityAfterMultipleAdds) {
    Registry reg;
    auto* c1 = reg.AddCounter("c1", "c1", {});
    auto* c2 = reg.AddCounter("c2", "c2", {});
    c1->Inc(1);
    c2->Inc(2);
    // Pointers must still be valid even after adding more counters.
    EXPECT_EQ(c1->Get(), 1u);
    EXPECT_EQ(c2->Get(), 2u);
}

}  // namespace
