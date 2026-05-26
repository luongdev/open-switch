/*
 * src/control/server.cc — osw::control::GrpcServer implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/server.h"

#include <chrono>
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

GrpcServer::GrpcServer(Health* health) noexcept
    : health_(health) {}

GrpcServer::~GrpcServer() noexcept {
    // Defensive: if Drain wasn't called explicitly, do it now with a
    // 0s deadline so the destructor doesn't block indefinitely.
    Drain(std::chrono::system_clock::now() + std::chrono::seconds(2));
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
    // If operator used port 0, the actual bound port is in bound_port.
    // Preserve the original config string but log the resolved port for
    // diagnostics.
    osw::log::Info("control",
                   "gRPC server listening on %s (bound port=%d)",
                   bound_address_.c_str(), bound_port);

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
    }
    if (worker.joinable()) {
        worker.join();
    }
    osw::log::Info("control", "gRPC server drained");
}

std::string GrpcServer::BoundAddress() const noexcept {
    return bound_address_;
}

}  // namespace osw::control
