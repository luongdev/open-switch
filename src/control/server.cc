/*
 * src/control/server.cc — osw::control::GrpcServer implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/server.h"

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

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
    static constexpr char kPrefix[] =
        "[osw:control] ~GrpcServer Drain threw: ";
    (void)::write(STDERR_FILENO, kPrefix, sizeof(kPrefix) - 1);
    if (msg) {
        (void)::write(STDERR_FILENO, msg, std::strlen(msg));
    }
    (void)::write(STDERR_FILENO, "\n", 1);
}

}  // namespace

GrpcServer::GrpcServer(Health* health) noexcept
    : health_(health) {}

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

void GrpcServer::SetVersions(std::string module_version,
                             std::string freeswitch_version) {
    module_version_     = std::move(module_version);
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

    grpc::ServerBuilder builder;
    int bound_port = 0;
    builder.AddListeningPort(config.grpc_listen_address, creds_, &bound_port);
    builder.RegisterService(service_.get());
    // Note: bound_port is filled in by BuildAndStart() (the resolver
    // runs there, not at AddListeningPort registration). We capture
    // it into the member after BuildAndStart succeeds.
    if (config.grpc_max_concurrent_streams > 0) {
        builder.SetResourceQuota(grpc::ResourceQuota("osw_quota"));
        builder.AddChannelArgument(
            GRPC_ARG_MAX_CONCURRENT_STREAMS,
            static_cast<int>(config.grpc_max_concurrent_streams));
    }

    server_ = builder.BuildAndStart();
    if (!server_) {
        osw::log::Error("control",
                        "BuildAndStart failed for address %s",
                        config.grpc_listen_address.c_str());
        service_.reset();
        creds_.reset();
        return false;
    }
    bound_address_ = config.grpc_listen_address;
    bound_port_    = bound_port;
    // If operator used port 0, the actual bound port is in bound_port_.
    // Preserve the original config string but log the resolved port for
    // diagnostics.
    osw::log::Info("control",
                   "gRPC server listening on %s (bound port=%d)",
                   bound_address_.c_str(), bound_port_);

    // Worker thread that blocks in Wait(). Drain joins it.
    worker_ = std::thread([this]() {
        try {
            server_->Wait();
        } catch (const std::exception& e) {
            osw::log::Error("control",
                            "grpc::Server::Wait threw: %s", e.what());
        } catch (...) {
            osw::log::Error("control",
                            "grpc::Server::Wait threw an unknown exception");
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

}  // namespace osw::control
