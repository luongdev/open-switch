/*
 * tests/unit/control/rpc_metrics_test.cc
 *
 * Unit tests for osw::control::RpcMetrics and associated helpers.
 * No real gRPC runtime needed — tests call Record/RecordAuthz directly
 * and inspect the prometheus::Registry output.
 *
 * Covered:
 *   - ExtractRpcName: fully-qualified → short; no slash; empty.
 *   - StatusCodeToString: OK, CANCELLED, UNIMPLEMENTED, unknown.
 *   - RpcMetrics::Record: increments osw_rpc_calls_total and
 *     osw_rpc_latency_seconds.
 *   - RpcMetrics::RecordAuthz: increments osw_rpc_authz_decisions_total.
 *   - Counter per (rpc, code) pair is distinct.
 *   - Calling Record multiple times for the same (rpc, code) accumulates.
 *   - Prometheus page from Registry::Render contains expected metric names.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/rpc_metrics.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "osw/observability/prometheus.h"

namespace {

using namespace osw::control;
using namespace osw::observability::prometheus;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static bool PageContains(std::string_view page, std::string_view needle) {
    return page.find(needle) != std::string_view::npos;
}

// ---------------------------------------------------------------------------
// ExtractRpcName
// ---------------------------------------------------------------------------

TEST(ExtractRpcNameTest, FullyQualifiedPath) {
    EXPECT_EQ(ExtractRpcName("/open_switch.control.v1.ControlService/Health"), "Health");
}

TEST(ExtractRpcNameTest, AnotherMethod) {
    EXPECT_EQ(ExtractRpcName("/open_switch.control.v1.ControlService/Originate"), "Originate");
}

TEST(ExtractRpcNameTest, NoSlashReturnsWholeString) {
    EXPECT_EQ(ExtractRpcName("Health"), "Health");
}

TEST(ExtractRpcNameTest, EmptyStringReturnsEmpty) {
    EXPECT_EQ(ExtractRpcName(""), "");
}

TEST(ExtractRpcNameTest, TrailingSlashReturnsEmpty) {
    // "/pkg/svc/" — trailing slash → empty method name returned.
    EXPECT_EQ(ExtractRpcName("/pkg/svc/"), "");
}

// ---------------------------------------------------------------------------
// StatusCodeToString
// ---------------------------------------------------------------------------

TEST(StatusCodeToStringTest, OK) {
    EXPECT_EQ(StatusCodeToString(grpc::StatusCode::OK), "OK");
}

TEST(StatusCodeToStringTest, Cancelled) {
    EXPECT_EQ(StatusCodeToString(grpc::StatusCode::CANCELLED), "CANCELLED");
}

TEST(StatusCodeToStringTest, Unimplemented) {
    EXPECT_EQ(StatusCodeToString(grpc::StatusCode::UNIMPLEMENTED), "UNIMPLEMENTED");
}

TEST(StatusCodeToStringTest, PermissionDenied) {
    EXPECT_EQ(StatusCodeToString(grpc::StatusCode::PERMISSION_DENIED), "PERMISSION_DENIED");
}

// ---------------------------------------------------------------------------
// RpcMetrics
// ---------------------------------------------------------------------------

class RpcMetricsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        registry_ = std::make_unique<Registry>();
        metrics_ = std::make_unique<RpcMetrics>(registry_.get());
    }

    std::unique_ptr<Registry> registry_;
    std::unique_ptr<RpcMetrics> metrics_;
};

TEST_F(RpcMetricsTest, RecordCreatesCallCounterAndLatencyHistogram) {
    metrics_->Record("Health", grpc::StatusCode::OK, 0.001);
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page, "osw_rpc_calls_total"));
    EXPECT_TRUE(PageContains(page, "osw_rpc_latency_seconds"));
}

TEST_F(RpcMetricsTest, RecordIncrementsCallCounter) {
    metrics_->Record("Health", grpc::StatusCode::OK, 0.001);
    metrics_->Record("Health", grpc::StatusCode::OK, 0.002);
    const auto page = registry_->Render();
    // Two calls to Health/OK → counter should be 2.
    EXPECT_TRUE(PageContains(page, "osw_rpc_calls_total{rpc=\"Health\",code=\"OK\"} 2"));
}

TEST_F(RpcMetricsTest, RecordDifferentStatusCodesProduceSeparateCounters) {
    metrics_->Record("Originate", grpc::StatusCode::OK, 0.05);
    metrics_->Record("Originate", grpc::StatusCode::INVALID_ARGUMENT, 0.001);
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page,
                             "osw_rpc_calls_total{rpc=\"Originate\",code=\"OK\"} 1"));
    EXPECT_TRUE(PageContains(page,
                             "osw_rpc_calls_total{rpc=\"Originate\","
                             "code=\"INVALID_ARGUMENT\"} 1"));
}

TEST_F(RpcMetricsTest, RecordDifferentRpcsProduceSeparateHistograms) {
    metrics_->Record("Health", grpc::StatusCode::OK, 0.001);
    metrics_->Record("Hangup", grpc::StatusCode::OK, 0.010);
    const auto page = registry_->Render();
    // Both RPC names should appear as rpc labels on the histogram.
    EXPECT_TRUE(PageContains(page, "rpc=\"Health\""));
    EXPECT_TRUE(PageContains(page, "rpc=\"Hangup\""));
}

TEST_F(RpcMetricsTest, LatencyHistogramCountMatchesObservations) {
    metrics_->Record("Bridge", grpc::StatusCode::OK, 0.05);
    metrics_->Record("Bridge", grpc::StatusCode::OK, 0.10);
    metrics_->Record("Bridge", grpc::StatusCode::OK, 0.25);
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page, "osw_rpc_latency_seconds_count{rpc=\"Bridge\"} 3"));
}

TEST_F(RpcMetricsTest, RecordAuthzCreatesAuthzCounter) {
    metrics_->RecordAuthz("Health", "allow");
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page, "osw_rpc_authz_decisions_total"));
    EXPECT_TRUE(PageContains(page,
                             "osw_rpc_authz_decisions_total{rpc=\"Health\",outcome=\"allow\"} 1"));
}

TEST_F(RpcMetricsTest, RecordAuthzDenyAndAllow) {
    metrics_->RecordAuthz("Originate", "allow");
    metrics_->RecordAuthz("Originate", "deny");
    metrics_->RecordAuthz("Originate", "deny");
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page,
                             "osw_rpc_authz_decisions_total{rpc=\"Originate\","
                             "outcome=\"allow\"} 1"));
    EXPECT_TRUE(PageContains(page,
                             "osw_rpc_authz_decisions_total{rpc=\"Originate\","
                             "outcome=\"deny\"} 2"));
}

TEST_F(RpcMetricsTest, SameRpcAndCodeAccumulatesIntoSingleCounter) {
    for (int i = 0; i < 5; ++i) {
        metrics_->Record("Hangup", grpc::StatusCode::OK, 0.001);
    }
    const auto page = registry_->Render();
    EXPECT_TRUE(PageContains(page, "osw_rpc_calls_total{rpc=\"Hangup\",code=\"OK\"} 5"));
}

}  // namespace
