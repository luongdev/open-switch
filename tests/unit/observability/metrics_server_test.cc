/*
 * tests/unit/observability/metrics_server_test.cc
 *
 * Unit tests for osw::observability::MetricsServer request handling.
 * All tests drive HandleRequest() directly — no real socket is opened.
 *
 * Covered:
 *   - GET /metrics → 200 OK with correct body and Content-Type.
 *   - GET /metrics with query string → 200 OK (query string stripped).
 *   - GET /other → 404 Not Found.
 *   - POST /metrics → 405 Method Not Allowed.
 *   - Empty request line → 400 Bad Request.
 *   - Malformed request line (no space) → 400 Bad Request.
 *   - Body from render_fn is included verbatim in the response.
 *   - Content-Length header value matches body length.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/metrics_server.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using osw::observability::MetricsServer;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static MetricsServer MakeServer(std::string fixed_body = "# no metrics\n") {
    return MetricsServer([b = std::move(fixed_body)]() { return b; });
}

static bool Contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Build a minimal HTTP/1.1 GET request string.
static std::string GetRequest(std::string_view path) {
    return std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MetricsServerTest, GetMetricsReturns200) {
    auto server = MakeServer("# some_metric 1\n");
    std::string response;
    const int code = server.HandleRequest(GetRequest("/metrics"), response);
    EXPECT_EQ(code, 200);
}

TEST(MetricsServerTest, GetMetricsResponseContainsOkStatus) {
    auto server = MakeServer();
    std::string response;
    server.HandleRequest(GetRequest("/metrics"), response);
    EXPECT_TRUE(Contains(response, "HTTP/1.1 200 OK"));
}

TEST(MetricsServerTest, GetMetricsBodyIsPropagated) {
    const std::string body =
        "# HELP osw_rpc_calls_total Total RPC calls\n"
        "osw_rpc_calls_total{rpc=\"Health\",code=\"OK\"} 42\n";
    auto server = MakeServer(body);
    std::string response;
    server.HandleRequest(GetRequest("/metrics"), response);
    EXPECT_TRUE(Contains(response, body));
}

TEST(MetricsServerTest, GetMetricsContentTypeIsPrometheusText) {
    auto server = MakeServer();
    std::string response;
    server.HandleRequest(GetRequest("/metrics"), response);
    EXPECT_TRUE(Contains(response, "Content-Type: text/plain"));
}

TEST(MetricsServerTest, GetMetricsContentLengthMatchesBodySize) {
    const std::string body = "abc_total 7\n";  // 12 bytes
    auto server = MakeServer(body);
    std::string response;
    server.HandleRequest(GetRequest("/metrics"), response);
    EXPECT_TRUE(Contains(response, "Content-Length: " + std::to_string(body.size())));
}

TEST(MetricsServerTest, GetMetricsWithQueryStringReturns200) {
    auto server = MakeServer();
    std::string response;
    const int code = server.HandleRequest(GetRequest("/metrics?foo=bar"), response);
    EXPECT_EQ(code, 200);
}

TEST(MetricsServerTest, GetOtherPathReturns404) {
    auto server = MakeServer();
    std::string response;
    const int code = server.HandleRequest(GetRequest("/healthz"), response);
    EXPECT_EQ(code, 404);
    EXPECT_TRUE(Contains(response, "HTTP/1.1 404 Not Found"));
}

TEST(MetricsServerTest, GetRootReturns404) {
    auto server = MakeServer();
    std::string response;
    const int code = server.HandleRequest(GetRequest("/"), response);
    EXPECT_EQ(code, 404);
}

TEST(MetricsServerTest, PostMetricsReturns405) {
    auto server = MakeServer();
    std::string response;
    const int code =
        server.HandleRequest("POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n", response);
    EXPECT_EQ(code, 405);
    EXPECT_TRUE(Contains(response, "405 Method Not Allowed"));
}

TEST(MetricsServerTest, EmptyRequestReturns400) {
    auto server = MakeServer();
    std::string response;
    const int code = server.HandleRequest("", response);
    EXPECT_EQ(code, 400);
}

TEST(MetricsServerTest, MalformedRequestLineNoSpaceReturns400) {
    auto server = MakeServer();
    std::string response;
    const int code = server.HandleRequest("NOSPACE\r\nHost: x\r\n\r\n", response);
    EXPECT_EQ(code, 400);
}

TEST(MetricsServerTest, HeadMethodReturns405) {
    auto server = MakeServer();
    std::string response;
    const int code =
        server.HandleRequest("HEAD /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n", response);
    EXPECT_EQ(code, 405);
}

TEST(MetricsServerTest, IsRunningFalseBeforeStart) {
    auto server = MakeServer();
    EXPECT_FALSE(server.IsRunning());
}

TEST(MetricsServerTest, StopWithoutStartIsNoop) {
    auto server = MakeServer();
    EXPECT_NO_THROW(server.Stop());
    EXPECT_FALSE(server.IsRunning());
}

}  // namespace
