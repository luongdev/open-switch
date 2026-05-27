/*
 * src/observability/metrics_server.cc
 *
 * Implementation of osw::observability::MetricsServer.
 * See include/osw/observability/metrics_server.h.
 *
 * POSIX APIs used:
 *   socket(), bind(), listen(), accept(), recv(), send(), close()
 *   inet_pton() / htons() for address parsing.
 *   setsockopt(SO_REUSEADDR) so the port is immediately reusable after
 *   Stop() in tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/metrics_server.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "osw/observability/log.h"

namespace osw::observability {

namespace {

constexpr const char* kSubsystem = "metrics.server";

// Maximum request size we'll read from one connection (8 KB is generous
// for a scraper that only sends GET /metrics HTTP/1.1\r\nHost:...\r\n).
constexpr std::size_t kMaxRequestBytes = 8192;

constexpr std::string_view kMetricsPath = "/metrics";
constexpr std::string_view kOkHeader =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
    "Connection: close\r\n";
constexpr std::string_view kNotFoundResponse =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "Content-Length: 9\r\n"
    "\r\n"
    "Not Found";
constexpr std::string_view kBadRequestResponse =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "Bad Request";
constexpr std::string_view kMethodNotAllowedResponse =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "Content-Length: 18\r\n"
    "\r\n"
    "Method Not Allowed";

// Write all bytes of `data` to `fd`. Returns false if interrupted or
// the connection was reset.
bool WriteAll(int fd, std::string_view data) noexcept {
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ptr += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// MetricsServer
// ---------------------------------------------------------------------------

MetricsServer::MetricsServer(std::function<std::string()> render_fn)
    : render_fn_(std::move(render_fn)) {}

MetricsServer::~MetricsServer() {
    Stop();
}

bool MetricsServer::Start(std::string_view bind_addr, std::uint16_t port) {
    if (running_.load(std::memory_order_acquire)) {
        return true;  // already running
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        osw::log::Error(kSubsystem, "socket() failed: %s", std::strerror(errno));
        return false;
    }

    // Allow rapid port reuse across test cycles.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // Parse the bind address.
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    const std::string addr_str(bind_addr);
    if (::inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr) != 1) {
        osw::log::Error(kSubsystem, "inet_pton failed for address '%s'", addr_str.c_str());
        ::close(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        osw::log::Error(kSubsystem,
                        "bind() on %s:%u failed: %s",
                        addr_str.c_str(),
                        static_cast<unsigned>(port),
                        std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (::listen(fd, 8) != 0) {
        osw::log::Error(kSubsystem, "listen() failed: %s", std::strerror(errno));
        ::close(fd);
        return false;
    }

    listen_fd_ = fd;
    running_.store(true, std::memory_order_release);
    loop_ = std::thread([this]() { Run(); });

    osw::log::Info(kSubsystem,
                   "Metrics HTTP server listening on %s:%u",
                   addr_str.c_str(),
                   static_cast<unsigned>(port));
    return true;
}

void MetricsServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // was already stopped
    }
    // Closing the listen socket causes the blocking accept() in Run()
    // to return with an error, which breaks the loop.
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (loop_.joinable()) {
        loop_.join();
    }
    osw::log::Info(kSubsystem, "Metrics HTTP server stopped");
}

void MetricsServer::Run() {
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        const int conn_fd =
            ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (conn_fd < 0) {
            if (!running_.load(std::memory_order_acquire)) {
                // We closed listen_fd_ in Stop(); accept() failing here is
                // expected — just exit the loop.
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            osw::log::Warn(kSubsystem, "accept() error: %s", std::strerror(errno));
            continue;
        }
        HandleConnection(conn_fd);
    }
}

void MetricsServer::HandleConnection(int fd) const {
    std::string response;
    std::array<char, kMaxRequestBytes + 1> buf{};

    // Read the request. We only need the first line to identify the
    // method and path; we don't bother parsing headers beyond looking
    // for \r\n\r\n to confirm the request is complete.
    std::size_t total = 0;
    bool got_end = false;
    while (total < kMaxRequestBytes) {
        const ssize_t n = ::recv(fd, buf.data() + total, kMaxRequestBytes - total, 0);
        if (n <= 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
        // Check for end-of-headers marker.
        if (std::string_view(buf.data(), total).find("\r\n\r\n") != std::string_view::npos) {
            got_end = true;
            break;
        }
    }

    const std::string_view raw(buf.data(), total);
    HandleRequest(raw, response);
    (void)WriteAll(fd, response);
    ::close(fd);
    (void)got_end;  // suppress unused-variable warning
}

int MetricsServer::HandleRequest(std::string_view raw_request, std::string& response) const {
    // Parse the first line: "<METHOD> <PATH> HTTP/1.x\r\n"
    const auto line_end = raw_request.find("\r\n");
    const auto request_line =
        (line_end == std::string_view::npos) ? raw_request : raw_request.substr(0, line_end);

    if (request_line.empty()) {
        response = kBadRequestResponse;
        return 400;
    }

    // Split method and path.
    const auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) {
        response = kBadRequestResponse;
        return 400;
    }
    const auto method = request_line.substr(0, sp1);

    const auto sp2 = request_line.find(' ', sp1 + 1);
    const auto path = (sp2 == std::string_view::npos) ? request_line.substr(sp1 + 1)
                                                      : request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Only GET is supported.
    if (method != "GET") {
        response = kMethodNotAllowedResponse;
        return 405;
    }

    // Only /metrics is served.
    // Strip any query string from path for comparison.
    const auto path_clean =
        (path.find('?') != std::string_view::npos) ? path.substr(0, path.find('?')) : path;

    if (path_clean != kMetricsPath) {
        response = kNotFoundResponse;
        return 404;
    }

    // Render the metrics page.
    const std::string body = render_fn_();
    const std::string content_length = std::to_string(body.size());

    response.reserve(kOkHeader.size() + 32 + body.size());
    response = kOkHeader;
    response += "Content-Length: ";
    response += content_length;
    response += "\r\n\r\n";
    response += body;
    return 200;
}

}  // namespace osw::observability
