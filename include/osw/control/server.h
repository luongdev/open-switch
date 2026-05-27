/*
 * include/osw/control/server.h
 *
 * osw::control::GrpcServer — synchronous gRPC server wrapping the
 * generated ControlService::Service base class. W1 implements only
 * the Health RPC; the other RPCs return UNIMPLEMENTED via a shared
 * helper.
 *
 * Lifecycle:
 *   - Start(address, creds) calls grpc::ServerBuilder, binds the
 *     address, registers the ControlServiceSkeleton, starts the
 *     server on a worker thread.
 *   - Drain(deadline) calls grpc::Server::Shutdown(deadline). Safe
 *     to call multiple times.
 *
 * The server holds the grpc::Server via std::unique_ptr so the
 * destructor automatically tears down on Module shutdown (defensive;
 * Module always Drains explicitly first).
 *
 * Threading:
 *   - Start spawns one worker thread that calls server_->Wait().
 *   - RPC handlers run in gRPC's internal thread pool.
 *   - Drain is safe from any thread (grpc::Server::Shutdown is
 *     thread-safe).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_SERVER_H_
#define OSW_CONTROL_SERVER_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>

namespace osw {
class Health;
struct Config;

namespace events {
class Broadcaster;
class RingSet;
}  // namespace events

namespace control {

class ControlServiceSkeleton;
class RpcMetrics;

class GrpcServer {
  public:
    /// `health` is the module-wide Health aggregator. The server holds
    /// a non-owning view; the Module owns the Health.
    explicit GrpcServer(Health* health) noexcept;
    ~GrpcServer() noexcept;

    GrpcServer(const GrpcServer&) = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;
    GrpcServer(GrpcServer&&) = delete;
    GrpcServer& operator=(GrpcServer&&) = delete;

    /// Builds + starts the gRPC server bound at `address`. The address
    /// format is "host:port" (matches grpc::ServerBuilder::AddListeningPort).
    /// Returns true on success.
    bool Start(const Config& config);

    /// Shut down the server with a deadline. Idempotent. Joins the
    /// worker thread before returning.
    void Drain(std::chrono::system_clock::time_point deadline);

    /// Returns the address the server is bound to (post-Start), or
    /// an empty string if not yet started. Useful for tests that
    /// pass "0.0.0.0:0" to grab a kernel-assigned port.
    [[nodiscard]] std::string BoundAddress() const noexcept;

    /// Returns the kernel-resolved TCP port the server bound to, or
    /// -1 if Start has not been called (or failed). When the operator
    /// passes "host:0" in grpc_listen_address, the actually-bound port
    /// is only knowable post-AddListeningPort; tests use this accessor
    /// to construct a client channel against the resolved port.
    [[nodiscard]] int BoundPort() const noexcept;

    /// Set the module + freeswitch version strings the Health RPC
    /// should report. Called by Module::Load before Start so that
    /// Snapshot includes the right values.
    void SetVersions(std::string module_version, std::string freeswitch_version);

    /// Inject the W2 event-plane bridges (Broadcaster + RingSet)
    /// into the ControlServiceSkeleton so that the SubscribeEvents
    /// handler can route. Called by Module::Load AFTER Broadcaster +
    /// RingSet are constructed and Start()-ed. Pre-W2 builds and
    /// tests that don't exercise SubscribeEvents leave these as
    /// nullptr; SubscribeEvents then returns UNIMPLEMENTED rather
    /// than crashing.
    ///
    /// Pointers are non-owning. The Module singleton outlives the
    /// gRPC server's RPC threads (Drain joins them before tearing
    /// down Module-owned subsystems).
    void SetEventPlane(events::Broadcaster* broadcaster,
                       events::RingSet* rings,
                       std::uint32_t max_subscribers,
                       std::uint32_t subscriber_send_queue_capacity) noexcept;

    /// Inject the W4C RpcMetrics collector. Must be called before Start().
    /// Non-owning pointer; the caller (Module) owns the RpcMetrics and the
    /// associated prometheus::Registry. When set, GrpcServer registers the
    /// RpcMetricsInterceptorFactory with the ServerBuilder so every RPC is
    /// timed and counted. When null (default), no metrics interceptor is
    /// installed (safe for tests that don't need metrics).
    void SetRpcMetrics(control::RpcMetrics* metrics) noexcept;

  private:
    Health* health_;
    std::shared_ptr<grpc::ServerCredentials> creds_;
    std::unique_ptr<ControlServiceSkeleton> service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread worker_;
    std::mutex drain_mu_;
    bool drained_ = false;
    std::string bound_address_;
    int bound_port_ = -1;
    std::string module_version_;
    std::string freeswitch_version_;
    control::RpcMetrics* rpc_metrics_ = nullptr;  // non-owning; may be null
};

}  // namespace control
}  // namespace osw

#endif  // OSW_CONTROL_SERVER_H_
