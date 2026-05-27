/*
 * include/osw/control/tls_reloader.h — inotify-based TLS cert hot-reloader.
 *
 * TlsReloader watches the directory that contains the server's cert and
 * key files. When the OS signals that a file in the directory was written
 * or renamed into place (atomic cert rotation), TlsReloader rebuilds the
 * grpc::ServerCredentials by calling BuildServerCredentials(TlsConfig) and
 * caches the validated credentials in an atomic slot. It also emits the
 * audit event "osw.control.tls.reload" on every successful rebuild.
 *
 * Design decisions (per W4 pre-decided answers):
 *
 *   OQ-5 — SIGHUP via inotify (not signal()):
 *     FreeSWITCH's SIGHUP handler only reloads XML config (FF-030); it does
 *     NOT re-invoke the module's load() function. Installing a POSIX signal()
 *     handler from inside a FreeSWITCH module is dangerous (FF-030 §"Implications").
 *     Instead we use inotify to watch the cert *directory* (not the inode)
 *     for IN_CLOSE_WRITE and IN_MOVED_TO events. Cert rotators (e.g. certbot)
 *     use atomic rename which changes the file inode; watching the directory
 *     handles both the direct-write and rename paths correctly.
 *
 *   Live credential swap:
 *     gRPC's standard ServerCredentials object does not support live rotation
 *     on an already-started server without the experimental SslServerCredentials
 *     "reload" callback (only available in some gRPC versions). W4 Track A
 *     uses the safe fallback: the rebuilt credentials are stored in
 *     rebuilt_creds_ and emitted via audit. Applying them requires the operator
 *     to restart the module (unload / load). The audit event
 *     "osw.control.tls.reload" notifies the operator that new certs are ready.
 *     W5 Track A may add live swap via grpc::experimental if the server is
 *     rebuilt in-place.
 *
 *   Platform portability:
 *     inotify is Linux-only. On non-Linux platforms the TlsReloader compiles
 *     to a no-op stub (Start/Stop are no-ops; HasNewCreds always returns false).
 *     Operators on non-Linux hosts must restart the module to apply new certs.
 *
 * Logger subsystem: "control.tls.reloader"
 * Audit:            "osw.control.tls.reload"
 *
 * Thread safety:
 *   - Start() and Stop() are NOT thread-safe relative to each other; the
 *     caller (Module::Load / Module::Shutdown) must ensure they are called
 *     from the same thread.
 *   - HasNewCreds() and SwapCreds() are thread-safe: they access rebuilt_creds_
 *     under a mutex.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_TLS_RELOADER_H_
#define OSW_CONTROL_TLS_RELOADER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/security/server_credentials.h>

#include "osw/control/tls_config.h"

namespace osw::control {

/// Callback type invoked by TlsReloader whenever new credentials have been
/// built and validated. The callee typically logs and stores the new creds.
using TlsReloadCallback =
    std::function<void(std::shared_ptr<grpc::ServerCredentials> new_creds)>;

/// inotify-backed cert-file watcher that rebuilds grpc::ServerCredentials
/// whenever a cert file changes on disk.
///
/// Lifecycle:
///   1. Construct with a TlsConfig that describes which cert/key/CA paths
///      to watch and a TlsReloadCallback that receives rebuilt credentials.
///   2. Call Start() from Module::Load (after GrpcServer::Start).
///   3. Call Stop() from Module::Shutdown (before GrpcServer::Drain).
///
/// On non-Linux platforms, Start() logs a warning and returns immediately;
/// no background thread is started.
class TlsReloader {
  public:
    /// Constructs a reloader for `cfg`. `on_reload` is called (synchronously
    /// from the background watcher thread) on each successful credential
    /// rebuild. Must not be nullptr.
    explicit TlsReloader(TlsConfig cfg, TlsReloadCallback on_reload);

    /// Destructor. Calls Stop() if Start() was called.
    ~TlsReloader();

    // Non-copyable, non-movable (owns an OS fd and a std::thread).
    TlsReloader(const TlsReloader&) = delete;
    TlsReloader& operator=(const TlsReloader&) = delete;
    TlsReloader(TlsReloader&&) = delete;
    TlsReloader& operator=(TlsReloader&&) = delete;

    /// Start the inotify watcher thread. No-op if TlsConfig::enabled()
    /// returns false or on non-Linux platforms. Safe to call more than once
    /// (second call is a no-op).
    void Start();

    /// Stop the watcher thread and release the inotify fd. Blocks until the
    /// background thread exits. Safe to call even if Start() was not called
    /// or on non-Linux platforms.
    void Stop() noexcept;

    /// Returns true iff TlsReloader is currently watching (Start() succeeded
    /// and Stop() has not been called).
    [[nodiscard]] bool IsRunning() const noexcept;

  private:
    /// Background thread entry-point. Calls RunLoop() and swallows exceptions.
    void ThreadFunc() noexcept;

    /// inotify poll loop. Returns when stop_ is set.
    void RunLoop();

    /// Extracts the directory part of a file path.
    static std::string DirOf(const std::string& path);

    /// Extracts the filename (basename) part of a file path.
    static std::string BaseOf(const std::string& path);

    TlsConfig cfg_;
    TlsReloadCallback on_reload_;

    // inotify file descriptor (-1 = not initialised / closed).
    int inotify_fd_ = -1;

    // Pipe used to wake the poll() loop when Stop() is called.
    int pipe_r_ = -1;
    int pipe_w_ = -1;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};

}  // namespace osw::control

#endif  // OSW_CONTROL_TLS_RELOADER_H_
