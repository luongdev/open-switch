/*
 * include/osw/control/auth_interceptor.h
 *
 * osw::control::AuthInterceptorFactory — gRPC ServerInterceptorFactoryInterface
 * that enforces mTLS-CN-preferred, JWT-fallback authentication and
 * per-RPC RBAC authorization on every incoming call.
 *
 * Design (per FF-029 and W4 spec):
 *
 *   Identity extraction order (first non-empty wins):
 *     1. mTLS client cert CN via
 *        `ctx->auth_context()->FindPropertyValues("x509_common_name")`.
 *     2. `Bearer <jwt>` from the `authorization` metadata header; if
 *        present, verifies ES256 signature using the cluster public key
 *        loaded from `RbacRegistry::jwt_public_key_path()`.
 *     3. None → identity = "anonymous".
 *
 *   Authorization:
 *     - Delegates to `RbacRegistry::Authorize(identity, rpc_path)`.
 *     - Denied calls: interceptor hijacks the RPC, emits a
 *       `osw.control.authz.deny` audit event, and returns either
 *       UNAUTHENTICATED (anonymous identity) or PERMISSION_DENIED
 *       (known identity, wrong permissions).
 *     - Allowed calls: emit `osw.control.authz.allow` audit event,
 *       then call `Proceed()`.
 *
 *   RBAC reload:
 *     - The factory holds `std::atomic<std::shared_ptr<RbacRegistry>>`.
 *     - `UpdateRegistry(new_registry)` atomically replaces the shared_ptr;
 *       the inotify watcher (W4 SIGHUP path) calls this.
 *     - Each per-RPC `AuthInterceptor` snapshots the shared_ptr at
 *       construction to use a consistent table for its lifetime.
 *
 * FF cited: FF-029 (interceptor lifecycle + thread model).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_AUTH_INTERCEPTOR_H_
#define OSW_CONTROL_AUTH_INTERCEPTOR_H_

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/support/server_interceptor.h>

#include "osw/control/jwt_verifier.h"
#include "osw/control/rbac.h"

namespace osw::control {

class AuthInterceptorFactory
    : public grpc::ServerInterceptorFactoryInterface {
  public:
    /// Construct with an initial RBAC registry and optional JWT verifier.
    /// `registry` must not be null.
    /// `jwt_verifier` may be null if JWT auth is not configured.
    explicit AuthInterceptorFactory(
        std::shared_ptr<RbacRegistry> registry,
        std::unique_ptr<JwtVerifier> jwt_verifier = nullptr) noexcept;

    ~AuthInterceptorFactory() override = default;

    AuthInterceptorFactory(const AuthInterceptorFactory&) = delete;
    AuthInterceptorFactory& operator=(const AuthInterceptorFactory&) = delete;

    /// Creates a per-RPC AuthInterceptor. Called by gRPC on each new RPC.
    /// Thread-safe: snapshots the current registry shared_ptr atomically.
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;

    /// Atomically replace the RBAC registry (SIGHUP reload path).
    void UpdateRegistry(std::shared_ptr<RbacRegistry> new_registry) noexcept;

    /// Atomically replace the JWT verifier (SIGHUP reload path).
    void UpdateJwtVerifier(std::unique_ptr<JwtVerifier> new_verifier) noexcept;

  private:
    std::atomic<std::shared_ptr<RbacRegistry>> registry_;
    // JWT verifier: shared_ptr so it can be atomically swapped.
    std::atomic<std::shared_ptr<JwtVerifier>> jwt_verifier_;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_AUTH_INTERCEPTOR_H_
