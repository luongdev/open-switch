/*
 * include/osw/observability/metrics_server.h
 *
 * osw::observability::MetricsServer — minimal HTTP/1.1 server that
 * exposes a Prometheus /metrics endpoint.
 *
 * Design:
 *   - Binds on a loopback address (default 127.0.0.1:9090). Operators
 *     expose via reverse proxy.
 *   - Single background thread; blocking accept-loop with thread-per-
 *     connection for each scrape (scrapes are infrequent — typically
 *     15s–60s intervals — so no epoll needed for V1).
 *   - GET /metrics → 200 OK + text/plain; charset=utf-8 body from
 *     prometheus::Registry::Render().
 *   - Any other method/path → 404 Not Found.
 *   - No keep-alive, no chunked encoding, no TLS (metrics are internal
 *     only; TLS termination is done by the operator's reverse proxy).
 *   - std::atomic<bool> running_ guards the accept loop; Stop() closes
 *     the listen socket which causes accept() to return EBADF/EINVAL,
 *     breaking the loop. The background thread is joined in Stop().
 *
 * Unit tests drive HandleConnection() directly (no real socket needed).
 *
 * Logger subsystem: "metrics.server"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_METRICS_SERVER_H_
#define OSW_OBSERVABILITY_METRICS_SERVER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "osw/observability/prometheus.h"

namespace osw::observability {

/// A minimal HTTP/1.1 server serving GET /metrics.
class MetricsServer {
  public:
    /// `render_fn` is called on every scrape to produce the response body.
    /// Typically: [&registry]() { return registry.Render(); }
    /// The function is stored and called from the server's connection
    /// handler thread — it must be thread-safe.
    explicit MetricsServer(std::function<std::string()> render_fn);
    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    /// Start listening on `bind_addr:port`. Returns true on success.
    /// The server background thread is started here. Calls after a
    /// successful Start() without an intervening Stop() are no-ops
    /// (returns true).
    bool Start(std::string_view bind_addr, std::uint16_t port);

    /// Stop the server. Closes the listen socket, waits for the
    /// background thread to join. Idempotent.
    void Stop();

    /// True after a successful Start() and before Stop().
    [[nodiscard]] bool IsRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // --- Testability API ---------------------------------------------------

    /// Parse an HTTP/1.1 request line and headers from `raw_request` and
    /// render a complete HTTP response into `response`. Used by unit tests
    /// to exercise the request-handling logic without a real socket.
    ///
    /// Returns the HTTP status code (200, 400, 404, …).
    int HandleRequest(std::string_view raw_request, std::string& response) const;

  private:
    /// Accept-loop run on the background thread.
    void Run();

    /// Handle one accepted connection on `fd`. Reads the HTTP request,
    /// calls HandleRequest, and writes the response. Closes `fd` before
    /// returning.
    void HandleConnection(int fd) const;

    std::function<std::string()> render_fn_;
    std::atomic<bool> running_{false};
    std::thread loop_;
    int listen_fd_ = -1;
};

}  // namespace osw::observability

#endif  // OSW_OBSERVABILITY_METRICS_SERVER_H_
