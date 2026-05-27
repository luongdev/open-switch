/*
 * include/osw/control/rpc_metrics.h
 *
 * osw::control::RpcMetrics — per-RPC Prometheus metrics collector.
 *
 * Wires into the gRPC server as a ServerInterceptor that records:
 *   - osw_rpc_calls_total{rpc, code}        — call count split by gRPC
 *                                             status code.
 *   - osw_rpc_latency_seconds{rpc}          — end-to-end RPC latency
 *                                             histogram.
 *   - osw_rpc_authz_decisions_total{rpc, outcome}
 *                                           — authz allow/deny counters
 *                                             (Track B populates these;
 *                                             Track C pre-registers them
 *                                             at zero).
 *
 * Design:
 *   - RpcMetrics owns one Counter per (rpc_name × grpc_status_code) pair
 *     and one Histogram per rpc_name. All metrics are registered into a
 *     caller-supplied prometheus::Registry at construction time.
 *   - RpcMetricsInterceptorFactory creates per-RPC interceptor instances
 *     compatible with grpc::experimental::ServerInterceptorFactoryInterface.
 *   - The interceptor measures wall-clock latency from
 *     PRE_SEND_INITIAL_METADATA to POST_SEND_STATUS and calls Record()
 *     on the shared RpcMetrics.
 *
 * RPC name convention: the method name from
 *   grpc::experimental::ServerRpcInfo::method()
 * which returns "/open_switch.control.v1.ControlService/Health" etc.
 * We strip the package prefix and use only the method part ("Health").
 *
 * Thread-safety: RpcMetrics is thread-safe (all mutations are atomic
 * via prometheus::Counter / prometheus::Histogram). The interceptor
 * instances are per-RPC and not shared across threads.
 *
 * Logger subsystem: "control.rpc_metrics"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_RPC_METRICS_H_
#define OSW_CONTROL_RPC_METRICS_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include "osw/observability/prometheus.h"

namespace osw::control {

// Forward declaration.
class RpcMetricsInterceptorFactory;

/// Holds all Prometheus metrics for the gRPC control plane.
///
/// Construction registers metrics into the supplied registry. The
/// registry must outlive this object (module-lifetime in practice).
class RpcMetrics {
  public:
    explicit RpcMetrics(osw::observability::prometheus::Registry* registry);

    // Non-copyable, non-movable (holds raw pointers into the registry).
    RpcMetrics(const RpcMetrics&) = delete;
    RpcMetrics& operator=(const RpcMetrics&) = delete;

    /// Record one completed RPC call.
    ///   - `rpc_name`: short method name ("Health", "Originate", …).
    ///   - `status_code`: gRPC status code (e.g. grpc::StatusCode::OK).
    ///   - `latency_seconds`: elapsed time from receipt to send-status.
    void Record(const std::string& rpc_name,
                grpc::StatusCode status_code,
                double latency_seconds);

    /// Record one authz decision. Called by Track B's auth interceptor.
    ///   - `outcome`: "allow" or "deny".
    void RecordAuthz(const std::string& rpc_name, const std::string& outcome);

    /// Create a ServerInterceptorFactory that attaches this collector to
    /// every RPC. The returned factory shares a pointer back to this
    /// RpcMetrics — the factory must not outlive this object.
    [[nodiscard]] std::unique_ptr<RpcMetricsInterceptorFactory> MakeFactory();

  private:
    // Get-or-create a counter for (rpc_name, status_code). Protected by mu_.
    osw::observability::prometheus::Counter* GetOrCreateCallCounter(
        const std::string& rpc_name, grpc::StatusCode code);

    // Get-or-create an authz counter for (rpc_name, outcome). Protected by mu_.
    osw::observability::prometheus::Counter* GetOrCreateAuthzCounter(
        const std::string& rpc_name, const std::string& outcome);

    // Get-or-create a latency histogram for rpc_name. Protected by mu_.
    osw::observability::prometheus::Histogram* GetOrCreateLatencyHistogram(
        const std::string& rpc_name);

    osw::observability::prometheus::Registry* registry_;  // non-owning

    mutable std::mutex mu_;

    // Keyed by "<rpc_name>/<status_code_int>".
    std::unordered_map<std::string, osw::observability::prometheus::Counter*> call_counters_;
    // Keyed by "<rpc_name>/<outcome>".
    std::unordered_map<std::string, osw::observability::prometheus::Counter*> authz_counters_;
    // Keyed by rpc_name.
    std::unordered_map<std::string, osw::observability::prometheus::Histogram*> latency_hists_;
};

// ---------------------------------------------------------------------------
// Interceptor + Factory
// ---------------------------------------------------------------------------

/// Per-RPC interceptor that records call count and latency.
class RpcMetricsInterceptor : public grpc::experimental::Interceptor {
  public:
    RpcMetricsInterceptor(RpcMetrics* metrics,
                          grpc::experimental::ServerRpcInfo* info);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

  private:
    RpcMetrics* metrics_;  // non-owning
    std::string rpc_name_;
    std::chrono::steady_clock::time_point start_;
    bool started_ = false;
};

/// Factory registered with the gRPC ServerBuilder. Creates one
/// RpcMetricsInterceptor per incoming RPC.
class RpcMetricsInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
  public:
    explicit RpcMetricsInterceptorFactory(RpcMetrics* metrics) noexcept
        : metrics_(metrics) {}

    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;

  private:
    RpcMetrics* metrics_;  // non-owning
};

/// Extract the short method name from a fully-qualified gRPC method path.
/// "/open_switch.control.v1.ControlService/Health" → "Health".
/// Returns the full string if no '/' separator is found.
[[nodiscard]] std::string ExtractRpcName(std::string_view method_path);

/// Convert a grpc::StatusCode to its string representation
/// ("OK", "CANCELLED", "UNKNOWN", …).
[[nodiscard]] std::string StatusCodeToString(grpc::StatusCode code);

}  // namespace osw::control

#endif  // OSW_CONTROL_RPC_METRICS_H_
