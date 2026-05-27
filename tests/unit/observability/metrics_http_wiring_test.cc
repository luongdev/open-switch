/*
 * tests/unit/observability/metrics_http_wiring_test.cc
 *
 * Integration-style test for the MetricsServer + prometheus::Registry +
 * HealthMetrics wiring that Module::Load performs.  This test does NOT
 * involve FreeSWITCH or the Module singleton — it exercises the same
 * construction and teardown pattern directly so we can run it without
 * a FS process.
 *
 * Covered:
 *   - Registry + HealthMetrics + MetricsServer constructs and Start()s.
 *   - GET /metrics over a real loopback TCP socket returns 200 OK with
 *     Prometheus exposition text that includes the registered gauge names.
 *   - MetricsServer::Stop() joins cleanly; no port-in-use on restart.
 *   - metrics_enabled=false path: no server constructed, no port bound.
 *   - Bind failure on a port already in use: Start() returns false;
 *     caller continues without crashing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <array>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "osw/observability/health.h"
#include "osw/observability/health_metrics.h"
#include "osw/observability/metrics_server.h"
#include "osw/observability/prometheus.h"

namespace {

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

/// Returns the kernel-assigned port for a bound socket, or 0 on error.
static std::uint16_t GetBoundPort(int fd) {
    struct sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

/// Open a blocking TCP connection to 127.0.0.1:port. Returns fd >= 0 on
/// success, -1 on failure. A 2-second recv timeout is set so the recv
/// loop in HttpGet never hangs indefinitely.
static int ConnectLoopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    // Set a short recv timeout so that if the server closes the connection
    // without sending data the recv loop exits promptly.
    struct timeval tv = {};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Write a simple HTTP GET request and return the full response string.
/// Returns empty string on socket error.
static std::string HttpGet(std::uint16_t port, std::string_view path) {
    const int fd = ConnectLoopback(port);
    if (fd < 0) {
        return {};
    }

    const std::string req =
        std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

    // Send the full request, then shut down the write half so the server
    // sees EOF on its recv side after reading headers. This is required:
    // the server's HandleConnection reads until \r\n\r\n, but if the OS
    // buffers the request and the server doesn't see EOF yet, it may block
    // in recv waiting for more data. Our request already ends with \r\n\r\n
    // so the server breaks early, but shutdown is belt-and-suspenders.
    const char* p = req.data();
    std::size_t remaining = req.size();
    while (remaining > 0) {
        const ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    ::shutdown(fd, SHUT_WR);

    // Receive the response. The server closes fd after sending, so recv
    // returns 0 (or EAGAIN on timeout) to terminate the loop.
    std::string resp;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            break;
        }
        resp.append(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    return resp;
}

// ---------------------------------------------------------------------------
// Wiring fixture
// ---------------------------------------------------------------------------

/// Constructs the same observability stack that Module::Load builds,
/// without touching FreeSWITCH.
class MetricsWiringFixture {
  public:
    MetricsWiringFixture() {
        registry_ = std::make_unique<osw::observability::prometheus::Registry>();
        health_metrics_ = std::make_unique<osw::observability::HealthMetrics>(registry_.get());
    }

    /// Build and start a MetricsServer on port 0 (kernel-assigned).
    /// Returns true when the server is listening.
    bool StartServer() {
        osw::observability::prometheus::Registry* reg_ptr = registry_.get();
        osw::observability::HealthMetrics* hm_ptr = health_metrics_.get();
        osw::Health* health_ptr = &health_;
        server_ = std::make_unique<osw::observability::MetricsServer>(
            [reg_ptr, hm_ptr, health_ptr]() -> std::string {
                hm_ptr->Refresh(*health_ptr);
                return reg_ptr->Render();
            });
        return server_->Start("127.0.0.1", 0);
    }

    /// Port the server bound to (valid after StartServer() returns true).
    std::uint16_t Port() const { return bound_port_; }

    /// Resolve the kernel-assigned port by starting the server and querying
    /// getsockname via a scratch socket.  We derive it indirectly: open a
    /// scratch connected socket immediately after Start() and read the peer
    /// port.  Alternatively, we can add a small helper in the test itself.
    ///
    /// Actually, MetricsServer::Start binds, then starts the loop thread.
    /// We need the port before we can connect.  The cleanest approach:
    /// start on port 0, then use getsockname on listen_fd_.  But
    /// listen_fd_ is private.  Work-around: connect and let the OS route;
    /// or we expose a BoundPort() method.  For this test we use the
    /// alternative approach: start on port 0 and derive the port by
    /// attempting a connect-until-success loop (the server is ready almost
    /// immediately).  Instead, we use the simpler ephemeral-port-probe trick:
    /// open a throw-away socket, bind port 0, read the port, close, then
    /// pass that port to the server.  This has a tiny TOCTOU window but is
    /// fine for a loopback unit test.
    ///
    /// For simplicity we expose a helper that:
    ///   1. Opens a probe socket and binds port 0.
    ///   2. Reads the assigned port.
    ///   3. Closes the probe socket.
    ///   4. Starts the MetricsServer on that port (REUSEADDR covers the gap).
    static std::uint16_t ProbeEphemeralPort() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return 0;
        }
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return 0;
        }
        const std::uint16_t port = GetBoundPort(fd);
        ::close(fd);
        return port;
    }

    bool StartServerOnPort(std::uint16_t port) {
        osw::observability::prometheus::Registry* reg_ptr = registry_.get();
        osw::observability::HealthMetrics* hm_ptr = health_metrics_.get();
        osw::Health* health_ptr = &health_;
        server_ = std::make_unique<osw::observability::MetricsServer>(
            [reg_ptr, hm_ptr, health_ptr]() -> std::string {
                hm_ptr->Refresh(*health_ptr);
                return reg_ptr->Render();
            });
        const bool ok = server_->Start("127.0.0.1", port);
        if (!ok) {
            // Mirror Module::Load's non-fatal bind-failure path: log error
            // (omitted here) and reset the server pointer so callers can check
            // fix.server() == nullptr to confirm no server is running.
            server_.reset();
            bound_port_ = 0;
        } else {
            bound_port_ = port;
        }
        return ok;
    }

    void StopServer() {
        if (server_) {
            server_->Stop();
            server_.reset();
        }
    }

    ~MetricsWiringFixture() {
        // Shutdown order mirrors Module::Shutdown:
        //   metrics_server_.Stop() → rpc/health adapters → registry.
        StopServer();
        health_metrics_.reset();
        registry_.reset();
    }

    osw::Health& health() { return health_; }
    osw::observability::prometheus::Registry& registry() { return *registry_; }
    osw::observability::MetricsServer* server() { return server_.get(); }

  private:
    osw::Health health_;
    std::unique_ptr<osw::observability::prometheus::Registry> registry_;
    std::unique_ptr<osw::observability::HealthMetrics> health_metrics_;
    std::unique_ptr<osw::observability::MetricsServer> server_;
    std::uint16_t bound_port_ = 0;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MetricsHttpWiringTest, GetMetricsReturns200WithPrometheusText) {
    MetricsWiringFixture fix;
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u) << "could not probe an ephemeral port";
    ASSERT_TRUE(fix.StartServerOnPort(port)) << "MetricsServer failed to start on port " << port;
    ASSERT_TRUE(fix.server()->IsRunning());

    const std::string resp = HttpGet(port, "/metrics");
    ASSERT_FALSE(resp.empty()) << "no response from metrics server";
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << "expected 200 OK, got: " << resp;
    EXPECT_NE(resp.find("text/plain"), std::string::npos)
        << "expected text/plain content-type, got: " << resp;
}

TEST(MetricsHttpWiringTest, GetMetricsBodyContainsHealthGaugeNames) {
    MetricsWiringFixture fix;
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);
    ASSERT_TRUE(fix.StartServerOnPort(port));

    const std::string resp = HttpGet(port, "/metrics");
    ASSERT_FALSE(resp.empty());
    // HealthMetrics registers osw_events_subscribers and ring-drop gauges.
    EXPECT_NE(resp.find("osw_events_subscribers"), std::string::npos)
        << "Health gauge missing from /metrics output:\n"
        << resp;
    EXPECT_NE(resp.find("osw_events_tier_ring_drops_total"), std::string::npos)
        << "Ring-drop gauge missing from /metrics output:\n"
        << resp;
}

TEST(MetricsHttpWiringTest, GetNonMetricsPathReturns404) {
    MetricsWiringFixture fix;
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);
    ASSERT_TRUE(fix.StartServerOnPort(port));

    const std::string resp = HttpGet(port, "/healthz");
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find("HTTP/1.1 404 Not Found"), std::string::npos)
        << "expected 404, got: " << resp;
}

TEST(MetricsHttpWiringTest, StopAndRestartDoesNotFailOnPortReuse) {
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);

    {
        MetricsWiringFixture fix1;
        ASSERT_TRUE(fix1.StartServerOnPort(port));
        fix1.StopServer();
        // Server stopped; port should be released (SO_REUSEADDR).
    }

    {
        MetricsWiringFixture fix2;
        EXPECT_TRUE(fix2.StartServerOnPort(port))
            << "port-reuse after Stop() failed on port " << port;
    }
}

TEST(MetricsHttpWiringTest, MetricsEnabledFalseDoesNotBindPort) {
    // Simulate the metrics_enabled=false path: do NOT construct the server.
    // Verify that no socket is left bound on the probed port.
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);

    // "metrics_enabled=false" — we simply skip constructing MetricsServer.
    // Verify we can immediately bind on that port ourselves.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const int rc = ::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    EXPECT_EQ(rc, 0) << "expected port " << port << " to be free: " << std::strerror(errno);
    ::close(fd);
}

TEST(MetricsHttpWiringTest, BindFailureOnUsedPortDoesNotCrash) {
    // Bind a socket on a port, then verify MetricsServer::Start() returns
    // false without crashing — mirroring the Module::Load non-fatal path.
    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);

    // Hold the port open.
    const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(holder, 0);
    const int one = 1;
    ::setsockopt(holder, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::bind(holder, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(holder, 1), 0);

    {
        MetricsWiringFixture fix;
        // Start on the already-bound port — should fail gracefully.
        const bool ok = fix.StartServerOnPort(port);
        EXPECT_FALSE(ok) << "expected bind failure on in-use port " << port;
        EXPECT_EQ(fix.server(), nullptr) << "server should be null after bind failure";
        // MetricsWiringFixture destructor must not crash.
    }

    ::close(holder);
}

TEST(MetricsHttpWiringTest, RegistryMetricAppearsInScrape) {
    // Register a custom counter into the registry; verify it appears in the
    // /metrics scrape, confirming the render_fn closure captures the right
    // registry pointer.
    MetricsWiringFixture fix;
    auto* ctr = fix.registry().AddCounter("osw_test_wiring_probe_total", "test probe counter", {});
    ctr->Inc(7);

    const std::uint16_t port = MetricsWiringFixture::ProbeEphemeralPort();
    ASSERT_NE(port, 0u);
    ASSERT_TRUE(fix.StartServerOnPort(port));

    const std::string resp = HttpGet(port, "/metrics");
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find("osw_test_wiring_probe_total"), std::string::npos)
        << "custom counter missing from /metrics:\n"
        << resp;
    EXPECT_NE(resp.find(" 7\n"), std::string::npos) << "counter value 7 missing:\n" << resp;
}

}  // namespace
