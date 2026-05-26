/*
 * tests/unit/control/server_test.cc
 *
 * In-process gRPC server tests. The server is started on
 * "127.0.0.1:0" (kernel-assigned port), then a client channel is
 * opened to the resolved port via BoundPort(). Per W1 contract
 * §"control/server_test.cc":
 *
 *   - Start binds, Health RPC returns SERVING with non-empty version,
 *     Shutdown(deadline) returns within deadline.
 *
 * The test uses InsecureChannelCredentials/InsecureServerCredentials
 * — TLS pathways are exercised separately in W3 once cert fixtures
 * land.
 *
 * Note: this test requires a real gRPC runtime. It runs only in the
 * CI builder container (where gRPC is installed); on dev hosts that
 * lack gRPC the entire control test target is excluded by CMake
 * (target builds against the production osw_control lib, which needs
 * gRPC).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "open_switch/control/v1/control.grpc.pb.h"
#include "osw/control/server.h"
#include "osw/core/config.h"
#include "osw/observability/health.h"

namespace {

class GrpcServerTest : public ::testing::Test {
 protected:
    void SetUp() override {
        health_ = std::make_unique<osw::Health>();
        health_->SetVersions("0.1.0-test", "FreeSWITCH 1.10.12-test");
        server_ = std::make_unique<osw::control::GrpcServer>(health_.get());
        server_->SetVersions("0.1.0-test", "FreeSWITCH 1.10.12-test");
        config_.grpc_listen_address = "127.0.0.1:0";  // kernel-assigned
        ASSERT_TRUE(server_->Start(config_));
        ASSERT_GT(server_->BoundPort(), 0)
            << "Kernel did not assign a port; gRPC bind likely failed";
    }
    void TearDown() override {
        const auto deadline = std::chrono::system_clock::now() +
                              std::chrono::seconds(1);
        server_->Drain(deadline);
    }

    // Constructs a client channel against the actually-bound port
    // (resolved post-BuildAndStart via GrpcServer::BoundPort).
    std::shared_ptr<grpc::Channel> OpenChannel() {
        const auto addr = std::string("127.0.0.1:") +
                          std::to_string(server_->BoundPort());
        return grpc::CreateChannel(addr,
                                   grpc::InsecureChannelCredentials());
    }

    osw::Config                                config_;
    std::unique_ptr<osw::Health>               health_;
    std::unique_ptr<osw::control::GrpcServer>  server_;
};

TEST_F(GrpcServerTest, BoundAddressReflectsConfig) {
    EXPECT_EQ(server_->BoundAddress(), "127.0.0.1:0");
}

TEST_F(GrpcServerTest, BoundPortIsAssignedByKernel) {
    EXPECT_GT(server_->BoundPort(), 0);
}

TEST_F(GrpcServerTest, DrainIsIdempotent) {
    const auto deadline = std::chrono::system_clock::now() +
                          std::chrono::seconds(1);
    server_->Drain(deadline);
    server_->Drain(deadline);  // second call is a no-op
    SUCCEED();
}

// The real W1 deliverable: a Health round-trip over the gRPC channel.
// This exercises ControlServiceSkeleton::Health, MapStatus, and the
// version-string plumbing end-to-end.
TEST_F(GrpcServerTest, RoundTripHealthReturnsServing) {
    auto channel = OpenChannel();
    ASSERT_TRUE(channel) << "OpenChannel returned null";

    // Block for up to 2 seconds while the channel connects, otherwise
    // the first RPC may race the worker thread's Wait() reaching the
    // poll loop.
    const auto connect_deadline = std::chrono::system_clock::now() +
                                  std::chrono::seconds(2);
    ASSERT_TRUE(channel->WaitForConnected(connect_deadline))
        << "Channel never reached READY against 127.0.0.1:"
        << server_->BoundPort();

    auto stub = open_switch::control::v1::ControlService::NewStub(channel);
    open_switch::control::v1::HealthRequest req;
    open_switch::control::v1::HealthResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(2));

    const grpc::Status status = stub->Health(&ctx, req, &resp);
    ASSERT_TRUE(status.ok())
        << "Health RPC failed: code=" << status.error_code()
        << " msg=" << status.error_message();

    EXPECT_EQ(resp.status(),
              open_switch::control::v1::HealthResponse::SERVING);
    EXPECT_FALSE(resp.module_version().empty());
    EXPECT_EQ(resp.module_version(), "0.1.0-test");
    EXPECT_EQ(resp.freeswitch_version(), "FreeSWITCH 1.10.12-test");
}

}  // namespace
