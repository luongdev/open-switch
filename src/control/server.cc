/*
 * src/control/server.cc — osw::control::GrpcServer implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/server.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "osw/control/auth_interceptor.h"
#include "osw/control/jwt_verifier.h"
#include "osw/control/rbac.h"
#include "osw/control/rpc_metrics.h"
#include "osw/control/tls.h"
#include "osw/core/config.h"
#include "osw/observability/health.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

// Signal-safe-ish diagnostic fallback for the destructor path. Drain
// allocates inside osw::log; if osw::log itself throws, calling another
// osw::log::Error from the catch handler would re-enter the allocator
// while the C++ runtime is mid-unwind. Use ::write(STDERR_FILENO, ...)
// which is async-signal-safe per POSIX and matches FF-012's documented
// signal-safe alternative ("use write(STDERR_FILENO, ...) instead").
//
// We cannot call switch_log_printf directly: osw_control is the
// FS-agnostic library (tests link it without <switch.h>). The
// production module's exception-boundary wrappers in mod_open_switch.cc
// already log via switch_log_printf at module-shutdown paths; this
// destructor is the last-line defence for the case where Module::Shutdown
// was skipped (e.g., FS aborted mid-load).
void WriteDestructorErrorRaw(const char* msg) noexcept {
    static constexpr char kPrefix[] = "[osw:control] ~GrpcServer Drain threw: ";
    (void)::write(STDERR_FILENO, kPrefix, sizeof(kPrefix) - 1);
    if (msg) {
        (void)::write(STDERR_FILENO, msg, std::strlen(msg));
    }
    (void)::write(STDERR_FILENO, "\n", 1);
}

}  // namespace

GrpcServer::GrpcServer(Health* health) noexcept : health_(health) {}

GrpcServer::~GrpcServer() noexcept {
    // Per designs/memory-management.md §"Exception-safety boundary":
    // C++ exceptions must NOT propagate out of a noexcept dtor. Drain
    // is NOT noexcept (osw::log::Info allocates; std::bad_alloc is
    // theoretically reachable). Wrap and swallow.
    //
    // Defensive: if Drain wasn't called explicitly, do it now with a
    // 2s deadline so the destructor doesn't block indefinitely.
    try {
        Drain(std::chrono::system_clock::now() + std::chrono::seconds(2));
    } catch (const std::exception& e) {
        WriteDestructorErrorRaw(e.what());
    } catch (...) {
        WriteDestructorErrorRaw("unknown");
    }
}

void GrpcServer::SetVersions(std::string module_version, std::string freeswitch_version) {
    module_version_ = std::move(module_version);
    freeswitch_version_ = std::move(freeswitch_version);
}

bool GrpcServer::Start(const Config& config) {
    creds_ = MakeServerCreds(config);
    if (!creds_) {
        osw::log::Error("control", "MakeServerCreds returned null; aborting Start");
        return false;
    }

    service_ = std::make_unique<ControlServiceSkeleton>(health_);
    service_->SetVersions(module_version_, freeswitch_version_);

    // --- W4 Track B: Build RBAC registry + auth interceptor --------------
    //
    // Parse the auth config from the embedded XML in config.auth_xml (if
    // present) or fall back to a default-deny empty registry.  The registry
    // is wrapped in the AuthInterceptorFactory which is registered with the
    // gRPC ServerBuilder below.
    //
    // JWT verifier: loaded from jwt_public_key_path if set.
    std::shared_ptr<RbacRegistry> rbac_registry;
    std::unique_ptr<JwtVerifier>  jwt_verifier;

    if (!config.auth_xml.empty()) {
        AuthConfig auth_cfg = ParseAuthConfig(config.auth_xml);
        if (!auth_cfg.jwt_public_key_path.empty()) {
            jwt_verifier = JwtVerifier::FromPemFile(auth_cfg.jwt_public_key_path);
            if (!jwt_verifier) {
                osw::log::Warn("control",
                               "Failed to load JWT public key from '%s'; "
                               "JWT auth disabled",
                               auth_cfg.jwt_public_key_path.c_str());
            }
        }
        rbac_registry = std::make_shared<RbacRegistry>(std::move(auth_cfg));
    } else {
        // No auth XML → default-deny registry (require=true, no roles).
        rbac_registry = std::make_shared<RbacRegistry>(AuthConfig{});
        osw::log::Warn("control",
                       "No <auth> config found; using default-deny RBAC "
                       "(all RPCs except anonymous Health.Check will be rejected)");
    }

    auth_factory_ = std::make_shared<AuthInterceptorFactory>(
        rbac_registry, std::move(jwt_verifier));

    grpc::ServerBuilder builder;
    int bound_port = 0;
    builder.AddListeningPort(config.grpc_listen_address, creds_, &bound_port);
    builder.RegisterService(service_.get());

    // Register all server-side interceptors in a single SetInterceptorCreators
    // call. The gRPC API REPLACES the interceptor vector on each call to this
    // method — there's no append — so Auth (W4B) and RpcMetrics (W4C) must be
    // batched here. Per FF-029: SetInterceptorCreators must be called before
    // BuildAndStart(); the factory pointer lifetime must exceed the server's
    // lifetime (auth_factory_ + rpc_metrics_ members satisfy this).
    //
    // Both factories conform to grpc::experimental::ServerInterceptorFactoryInterface
    // (gRPC 1.74 puts the server-side interceptor API in the `experimental`
    // namespace).
    {
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
            creators;

        // W4B: Auth + RBAC interceptor. We retain auth_factory_ as a
        // shared_ptr member so UpdateRegistry() (SIGHUP hot-reload) can
        // swap the RBAC table without restarting the server. Wrap with a
        // non-owning adapter so the SetInterceptorCreators contract
        // (unique_ptr ownership) is honoured while we keep our own ref.
        struct AuthFactoryAdapter
            : public grpc::experimental::ServerInterceptorFactoryInterface {
            explicit AuthFactoryAdapter(std::shared_ptr<AuthInterceptorFactory> f)
                : factory(std::move(f)) {}
            grpc::experimental::Interceptor* CreateServerInterceptor(
                grpc::experimental::ServerRpcInfo* info) override {
                return factory->CreateServerInterceptor(info);
            }
            std::shared_ptr<AuthInterceptorFactory> factory;
        };
        creators.push_back(std::make_unique<AuthFactoryAdapter>(auth_factory_));

        // W4C: RpcMetrics interceptor (optional — only when a collector
        // was injected via SetRpcMetrics).
        if (rpc_metrics_) {
            creators.push_back(rpc_metrics_->MakeFactory());
            osw::log::Info("control", "RpcMetrics interceptor registered");
        }

        builder.experimental().SetInterceptorCreators(std::move(creators));
    }
    // Note: bound_port is filled in by BuildAndStart() (the resolver
    // runs there, not at AddListeningPort registration). We capture
    // it into the member after BuildAndStart succeeds.
    if (config.grpc_max_concurrent_streams > 0) {
        builder.SetResourceQuota(grpc::ResourceQuota("osw_quota"));
        builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                                   static_cast<int>(config.grpc_max_concurrent_streams));
    }

    server_ = builder.BuildAndStart();
    if (!server_) {
        osw::log::Error(
            "control", "BuildAndStart failed for address %s", config.grpc_listen_address.c_str());
        service_.reset();
        creds_.reset();
        return false;
    }
    bound_address_ = config.grpc_listen_address;
    bound_port_ = bound_port;
    // If operator used port 0, the actual bound port is in bound_port_.
    // Preserve the original config string but log the resolved port for
    // diagnostics.
    osw::log::Info("control",
                   "gRPC server listening on %s (bound port=%d)",
                   bound_address_.c_str(),
                   bound_port_);

    // Worker thread that blocks in Wait(). Drain joins it.
    worker_ = std::thread([this]() {
        try {
            server_->Wait();
        } catch (const std::exception& e) {
            osw::log::Error("control", "grpc::Server::Wait threw: %s", e.what());
        } catch (...) {
            osw::log::Error("control", "grpc::Server::Wait threw an unknown exception");
        }
    });

    return true;
}

void GrpcServer::Drain(std::chrono::system_clock::time_point deadline) {
    std::unique_lock<std::mutex> lk(drain_mu_);
    if (drained_) {
        return;
    }
    drained_ = true;
    auto* srv = server_.get();
    auto& worker = worker_;
    lk.unlock();  // release the mutex while we block on Shutdown / join

    if (srv) {
        // grpc::Server::Shutdown is thread-safe and idempotent.
        srv->Shutdown(deadline);
        if (worker.joinable()) {
            worker.join();
        }
        osw::log::Info("control", "gRPC server drained");
    } else if (worker.joinable()) {
        // Defensive: shouldn't happen (server_ and worker_ are
        // populated together by Start), but if Start was partially
        // initialised, still join.
        worker.join();
    }
}

std::string GrpcServer::BoundAddress() const noexcept {
    return bound_address_;
}

int GrpcServer::BoundPort() const noexcept {
    return bound_port_;
}

void GrpcServer::SetRpcMetrics(control::RpcMetrics* metrics) noexcept {
    rpc_metrics_ = metrics;
}

void GrpcServer::SetEventPlane(events::Broadcaster* broadcaster,
                               events::RingSet* rings,
                               std::uint32_t max_subscribers,
                               std::uint32_t subscriber_send_queue_capacity) noexcept {
    // The service is constructed in Start(); SetEventPlane is called
    // by Module::Load AFTER Start() returns. If the operator called
    // us out of order (service_ still null), log and no-op — the
    // SubscribeEvents handler will return UNIMPLEMENTED until the
    // bridges are injected.
    if (!service_) {
        osw::log::Warn("control",
                       "GrpcServer::SetEventPlane called before Start; "
                       "SubscribeEvents will return UNIMPLEMENTED until rewired");
        return;
    }
    service_->SetEventPlane(broadcaster, rings, max_subscribers, subscriber_send_queue_capacity);
}

}  // namespace osw::control
