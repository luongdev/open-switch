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

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/control/idempotency_cache.h"
#include "osw/control/rpc_metrics.h"
#include "osw/control/tls.h"
#include "osw/core/config.h"
#include "osw/observability/health.h"
#include "osw/observability/log.h"

// Forward-declare MediaBugManager for the W6C setter pass-throughs.
// Full definition is in osw/media/bug_manager.h; server.cc only holds
// and passes pointers, so the forward-decl is sufficient.
namespace osw::media {
class MediaBugManager;
}

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

void GrpcServer::SetVersions(std::string_view module_version, std::string_view freeswitch_version) {
    module_version_ = module_version;
    freeswitch_version_ = freeswitch_version;
}

bool GrpcServer::Start(const Config& config) {
    creds_ = MakeServerCreds(config);
    if (!creds_) {
        osw::log::Error("control", "MakeServerCreds returned null; aborting Start");
        return false;
    }

    service_ = std::make_unique<ControlServiceSkeleton>(health_);
    service_->SetVersions(module_version_, freeswitch_version_);
    // Apply any cache pointer staged via SetIdempotencyCache(...) BEFORE
    // Start() — this closes the race window where the gRPC server is
    // already accepting RPCs but the skeleton's cache pointer is still
    // null (Gemini W5 P3-1). Module::Load now constructs the cache and
    // calls SetIdempotencyCache before Start(); this line applies it.
    if (pending_cache_) {
        service_->SetIdempotencyCache(pending_cache_);
    }
    // Apply W6C staged pointers with the same pattern.
    if (pending_bug_mgr_) {
        service_->SetMediaBugManager(pending_bug_mgr_);
    }
    if (pending_streams_) {
        service_->SetActiveMediaStreams(pending_streams_);
    }
    if (pending_bots_) {
        service_->SetActiveBots(pending_bots_);
    }
    if (pending_media_cfg_) {
        service_->SetConfig(pending_media_cfg_);
    }

    grpc::ServerBuilder builder;
    int bound_port = 0;
    builder.AddListeningPort(config.grpc_listen_address, creds_, &bound_port);
    builder.RegisterService(service_.get());

    // Register the RpcMetrics interceptor (W4C) when a collector was injected
    // via SetRpcMetrics. mod_open_switch is an internal control plane for the
    // tts/stt ecosystem — clients are trusted peers on the private network, so
    // there is no auth/RBAC interceptor here. Network-layer controls (firewall,
    // VPN, docker network isolation) are the boundary; mTLS is available via
    // BuildServerCredentials when operators want zero-trust between hosts.
    if (rpc_metrics_) {
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
            creators;
        creators.push_back(rpc_metrics_->MakeFactory());
        builder.experimental().SetInterceptorCreators(std::move(creators));
        osw::log::Info("control", "RpcMetrics interceptor registered");
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

void GrpcServer::SetIdempotencyCache(control::IdempotencyCache* cache) noexcept {
    // Stash the pointer so Start() can apply it the moment the skeleton is
    // constructed (closes the pre-Start race window). If the skeleton
    // already exists (post-Start call), apply immediately too.
    pending_cache_ = cache;
    if (service_) {
        service_->SetIdempotencyCache(cache);
    }
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

void GrpcServer::SetMediaBugManager(osw::media::MediaBugManager* mgr) noexcept {
    pending_bug_mgr_ = mgr;
    if (service_) {
        service_->SetMediaBugManager(mgr);
    }
}

void GrpcServer::SetActiveMediaStreams(osw::control::ActiveMediaStreams* streams) noexcept {
    pending_streams_ = streams;
    if (service_) {
        service_->SetActiveMediaStreams(streams);
    }
}

void GrpcServer::SetActiveBots(osw::control::ActiveBots* bots) noexcept {
    pending_bots_ = bots;
    if (service_) {
        service_->SetActiveBots(bots);
    }
}

void GrpcServer::SetMediaConfig(const osw::Config* config) noexcept {
    pending_media_cfg_ = config;
    if (service_) {
        service_->SetConfig(config);
    }
}

}  // namespace osw::control
