/*
 * src/control/tls_reloader.cc — inotify-based TLS cert hot-reloader.
 *
 * Decision OQ-5 (SIGHUP via inotify, not signal()):
 *   FreeSWITCH's SIGHUP handler reloads XML config only; it does NOT
 *   re-invoke module load/shutdown (FF-030). Installing a POSIX signal()
 *   handler from a module is unsafe. inotify is the correct mechanism.
 *
 * We watch the *directory* containing the cert/key files, not the file
 * inodes themselves. Cert rotators (certbot, acme.sh, Kubernetes secret
 * mounts) use atomic rename: the new cert lands as a temp file then is
 * renamed into place. Watching the inode would miss this (the inode
 * changes on rename). Watching the directory catches both:
 *   - IN_CLOSE_WRITE: direct write + close to the cert file.
 *   - IN_MOVED_TO: a file renamed into the watched directory.
 *
 * On non-Linux platforms the entire RunLoop() is a compile-time stub
 * controlled by the __linux__ guard. The external interface (Start/Stop)
 * compiles everywhere; only the watcher loop is Linux-only.
 *
 * Logger subsystem: "control.tls.reloader"
 * Audit:            "osw.control.tls.reload"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/tls_reloader.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#ifdef __linux__
#include <poll.h>
#include <unistd.h>

#include <sys/inotify.h>
#endif

#include "osw/control/tls.h"
#include "osw/control/tls_config.h"
#include "osw/observability/log.h"

// The audit helper is FS-dependent (it calls switch_event_create_subclass
// + switch_event_fire). We emit audit events here but keep them guarded so
// unit tests that compile tls_reloader.cc with OSW_TEST_FS_MOCK=1 do not
// pull in the FS API. When the mock is active, audit emits are silently
// skipped — the test validates the watcher logic, not the audit path.
#ifndef OSW_TEST_FS_MOCK
#include "osw/observability/audit.h"
#endif

namespace osw::control {

// ─── Helpers ────────────────────────────────────────────────────────────────

std::string TlsReloader::DirOf(const std::string& path) {
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return pos == 0 ? "/" : path.substr(0, pos);
}

std::string TlsReloader::BaseOf(const std::string& path) {
    const auto pos = path.rfind('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// ─── Constructor / Destructor ───────────────────────────────────────────────

TlsReloader::TlsReloader(TlsConfig cfg, TlsReloadCallback on_reload)
    : cfg_(std::move(cfg)), on_reload_(std::move(on_reload)) {}

TlsReloader::~TlsReloader() {
    Stop();
}

// ─── Start ──────────────────────────────────────────────────────────────────

void TlsReloader::Start() {
    if (running_.load(std::memory_order_acquire)) {
        return;  // already running
    }
    if (!cfg_.enabled()) {
        osw::log::Info("control.tls.reloader", "TLS not configured; cert hot-reload disabled");
        return;
    }

#ifndef __linux__
    osw::log::Warn("control.tls.reloader",
                   "inotify is not available on this platform; "
                   "cert hot-reload disabled — restart the module to apply new certs");
    return;
#else
    // Create a self-pipe for Stop() to wake the poll loop.
    int pfd[2] = {-1, -1};
    if (::pipe(pfd) != 0) {
        osw::log::Error("control.tls.reloader",
                        "pipe() failed: %s — cert hot-reload disabled",
                        std::strerror(errno));
        return;
    }
    pipe_r_ = pfd[0];
    pipe_w_ = pfd[1];

    // Create an inotify instance (close-on-exec, non-blocking).
    inotify_fd_ = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        osw::log::Error("control.tls.reloader",
                        "inotify_init1() failed: %s — cert hot-reload disabled",
                        std::strerror(errno));
        ::close(pipe_r_);
        ::close(pipe_w_);
        pipe_r_ = pipe_w_ = -1;
        return;
    }

    // Watch the directory containing the cert file. We use a single watch
    // on the cert directory; if key and CA live elsewhere we add additional
    // watches. De-duplicate to avoid adding the same directory twice.
    const std::string cert_dir = DirOf(cfg_.cert_path);
    const int wd = ::inotify_add_watch(inotify_fd_, cert_dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        osw::log::Error("control.tls.reloader",
                        "inotify_add_watch('%s') failed: %s — cert hot-reload disabled",
                        cert_dir.c_str(),
                        std::strerror(errno));
        ::close(inotify_fd_);
        ::close(pipe_r_);
        ::close(pipe_w_);
        inotify_fd_ = pipe_r_ = pipe_w_ = -1;
        return;
    }

    // Add watches for key and CA directories if they differ.
    if (!cfg_.key_path.empty()) {
        const std::string key_dir = DirOf(cfg_.key_path);
        if (key_dir != cert_dir) {
            if (::inotify_add_watch(inotify_fd_, key_dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) <
                0) {
                osw::log::Warn("control.tls.reloader",
                               "inotify_add_watch('%s') for key dir failed: %s",
                               key_dir.c_str(),
                               std::strerror(errno));
            }
        }
    }
    if (!cfg_.ca_path.empty()) {
        const std::string ca_dir = DirOf(cfg_.ca_path);
        if (ca_dir != cert_dir && ca_dir != DirOf(cfg_.key_path)) {
            if (::inotify_add_watch(inotify_fd_, ca_dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) <
                0) {
                osw::log::Warn("control.tls.reloader",
                               "inotify_add_watch('%s') for CA dir failed: %s",
                               ca_dir.c_str(),
                               std::strerror(errno));
            }
        }
    }

    stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { ThreadFunc(); });

    osw::log::Info("control.tls.reloader",
                   "cert hot-reload active — watching dir '%s' for cert '%s'",
                   cert_dir.c_str(),
                   BaseOf(cfg_.cert_path).c_str());
#endif  // __linux__
}

// ─── Stop ───────────────────────────────────────────────────────────────────

void TlsReloader::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // never started or already stopped
    }
    stop_.store(true, std::memory_order_release);

#ifdef __linux__
    // Wake the poll loop by writing one byte to the write end of the pipe.
    if (pipe_w_ >= 0) {
        const char c = 'q';
        (void)::write(pipe_w_, &c, 1);
    }
#endif

    if (worker_.joinable()) {
        worker_.join();
    }

#ifdef __linux__
    if (inotify_fd_ >= 0) {
        ::close(inotify_fd_);
        inotify_fd_ = -1;
    }
    if (pipe_r_ >= 0) {
        ::close(pipe_r_);
        pipe_r_ = -1;
    }
    if (pipe_w_ >= 0) {
        ::close(pipe_w_);
        pipe_w_ = -1;
    }
#endif

    osw::log::Info("control.tls.reloader", "cert hot-reload stopped");
}

bool TlsReloader::IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

// ─── ThreadFunc ─────────────────────────────────────────────────────────────

void TlsReloader::ThreadFunc() noexcept {
    try {
        RunLoop();
    } catch (const std::exception& e) {
        osw::log::Error("control.tls.reloader", "watcher thread threw: %s", e.what());
    } catch (...) {
        osw::log::Error("control.tls.reloader", "watcher thread threw an unknown exception");
    }
}

// ─── RunLoop ────────────────────────────────────────────────────────────────

void TlsReloader::RunLoop() {
#ifndef __linux__
    // Non-Linux stub — unreachable because Start() returns early on non-Linux.
    return;
#else
    const std::string cert_base = BaseOf(cfg_.cert_path);
    const std::string key_base = BaseOf(cfg_.key_path);
    const std::string ca_base = BaseOf(cfg_.ca_path);

    // poll(2) on two fds:
    //   fds[0] = inotify_fd_  — cert-directory events.
    //   fds[1] = pipe_r_      — Stop() wake signal.
    struct pollfd fds[2]{};
    fds[0].fd = inotify_fd_;
    fds[0].events = POLLIN;
    fds[1].fd = pipe_r_;
    fds[1].events = POLLIN;

    // Buffer large enough for several inotify_event structs plus the
    // variable-length name field (NAME_MAX = 255 on Linux).
    constexpr std::size_t kBufSize = 4096;
    char buf[kBufSize] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (!stop_.load(std::memory_order_acquire)) {
        const int ready = ::poll(fds, 2, /* timeout_ms = */ 5000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by signal; retry
            }
            osw::log::Error("control.tls.reloader", "poll() failed: %s", std::strerror(errno));
            break;
        }
        if (ready == 0) {
            continue;  // timeout — check stop_ and loop
        }

        // Stop pipe fired.
        if ((fds[1].revents & POLLIN) != 0) {
            break;
        }

        // inotify events available.
        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }

        const ssize_t n = ::read(inotify_fd_, buf, kBufSize);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            osw::log::Error(
                "control.tls.reloader", "read(inotify_fd) failed: %s", std::strerror(errno));
            break;
        }

        // Walk the event buffer and check whether any event names match
        // the cert, key, or CA filename.
        bool relevant = false;
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(n)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + offset);
            if (ev->len > 0) {
                const std::string name(ev->name, std::strlen(ev->name));
                if (name == cert_base || (!key_base.empty() && name == key_base) ||
                    (!ca_base.empty() && name == ca_base)) {
                    relevant = true;
                }
            }
            offset += sizeof(struct inotify_event) + ev->len;
        }

        if (!relevant) {
            continue;
        }

        osw::log::Info("control.tls.reloader",
                       "cert file changed; rebuilding credentials from %s",
                       cfg_.cert_path.c_str());

        // Rebuild credentials.
        auto new_creds = BuildServerCredentials(cfg_);
        if (!new_creds) {
            osw::log::Error("control.tls.reloader",
                            "credential rebuild failed after cert change — "
                            "keeping previous credentials");
            continue;
        }

        // Emit audit event "osw.control.tls.reload" (FF-030).
        // Decision OQ-5: audit emitted on every successful reload so
        // operators can correlate log lines with cert rotation events.
#ifndef OSW_TEST_FS_MOCK
        osw::audit::Emit("control.tls.reload",
                         {{"cert_path", cfg_.cert_path},
                          {"key_path", cfg_.key_path},
                          {"ca_path", cfg_.ca_path},
                          {"mtls", cfg_.require_client_cert ? "true" : "false"}});
#endif

        // Invoke the caller's callback with the new credentials.
        if (on_reload_) {
            on_reload_(std::move(new_creds));
        }
    }
#endif  // __linux__
}

}  // namespace osw::control
