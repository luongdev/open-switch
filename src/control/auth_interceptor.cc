/*
 * src/control/auth_interceptor.cc — AuthInterceptorFactory + per-RPC
 * AuthInterceptor implementation.
 *
 * Per FF-029:
 *   - PRE_RECV_INITIAL_METADATA is the hook point for auth decisions.
 *   - Allowed RPCs: call Proceed().
 *   - Denied RPCs: call Hijack(); in PRE_SEND_INITIAL_METADATA set an
 *     error via ServerContext::TryCancel() and then in PRE_SEND_STATUS
 *     deliver the appropriate gRPC status code.
 *
 * Audit:
 *   osw::audit::Emit("control.authz.allow", ...) — allowed path.
 *   osw::audit::Emit("control.authz.deny",  ...) — denied path.
 *
 * The per-RPC interceptor lives entirely on the gRPC worker thread
 * processing that RPC.  No locks needed in the interceptor itself.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/auth_interceptor.h"

#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/support/server_interceptor.h>

#include "osw/observability/audit.h"
#include "osw/observability/log.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem      = "control.auth";
constexpr const char* kAuthzAllow     = "control.authz.allow";
constexpr const char* kAuthzDeny      = "control.authz.deny";
constexpr const char* kAuthzMetaHeader = "authorization";

// ---------------------------------------------------------------------------
// Extract identity from ServerContext.
//   1. mTLS CN from auth_context.
//   2. Bearer JWT from metadata.
//   3. "anonymous".
// Returns the identity string and (via out-param) the JWT verifier result
// if JWT was used.
// ---------------------------------------------------------------------------
std::string ExtractIdentity(
    grpc::experimental::ServerRpcInfo* info,
    const std::shared_ptr<JwtVerifier>& jwt_verifier,
    bool& used_jwt) {
    used_jwt = false;
    (void)info;  // RpcInfo doesn't expose ServerContext directly — we store it
                 // at Intercept time when the hook fires with the ServerContext.
    // NOTE: This function is a placeholder signature; actual extraction
    // happens inside AuthInterceptor::Intercept() which has access to
    // the grpc::ServerContextBase via methods->GetServerContext() available
    // in grpc 1.74 experimental API.  See AuthInterceptor::Intercept below.
    return "anonymous";
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-RPC AuthInterceptor.
// ---------------------------------------------------------------------------

class AuthInterceptor : public grpc::experimental::Interceptor {
  public:
    AuthInterceptor(std::shared_ptr<RbacRegistry> registry,
                    std::shared_ptr<JwtVerifier>  jwt_verifier,
                    std::string                   rpc_method)
        : registry_(std::move(registry)),
          jwt_verifier_(std::move(jwt_verifier)),
          rpc_method_(std::move(rpc_method)) {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        using Hook = grpc::experimental::InterceptionHookPoints;

        if (methods->QueryInterceptionHookPoint(Hook::PRE_RECV_INITIAL_METADATA)) {
            // --- Identity extraction ---

            // Access ServerContext via the interceptor batch methods.
            // grpc::experimental::InterceptorBatchMethods provides
            // GetSendServerInitialMetadata / GetRecvInitialMetadata, but the
            // ServerContext itself is accessible via GetServerContext() in
            // gRPC 1.74 experimental surface.
            // Per grpcpp/support/server_interceptor.h (1.74), the batch
            // methods on the server side expose the ServerContext through the
            // interception framework context. We use the standard
            // grpc::ServerContextBase pointer obtainable from the RpcInfo
            // stored at factory time.
            //
            // Fallback: use the metadata map from GetRecvInitialMetadata().
            std::string identity = "anonymous";
            bool from_jwt = false;

            // Path 1: try to get mTLS CN from the server context if available.
            // grpc::experimental::InterceptorBatchMethods::GetInterceptedChannel()
            // is not applicable here (that's client-side). On the server side,
            // we retrieve the auth context from the server context pointer stored
            // in the ServerRpcInfo (accessible via the factory). However, gRPC's
            // server interceptor API in 1.74 does not directly expose
            // ServerContext* from InterceptorBatchMethods — we need to store the
            // ServerRpcInfo* (which has server_context()) at factory time.
            if (server_ctx_) {
                auto auth_ctx = server_ctx_->auth_context();
                if (auth_ctx) {
                    auto cns = auth_ctx->FindPropertyValues("x509_common_name");
                    if (!cns.empty() && !cns[0].empty()) {
                        identity = std::string(cns[0]);
                    }
                }
            }

            // Path 2: Bearer JWT fallback (when no mTLS CN found).
            if (identity == "anonymous" && jwt_verifier_) {
                auto* meta = methods->GetRecvInitialMetadata();
                if (meta) {
                    auto it = meta->find(kAuthzMetaHeader);
                    if (it != meta->end()) {
                        std::string_view val(it->second.data(), it->second.size());
                        constexpr std::string_view kBearer = "Bearer ";
                        if (val.starts_with(kBearer)) {
                            auto token = val.substr(kBearer.size());
                            auto result = jwt_verifier_->Verify(token);
                            if (result.ok) {
                                identity = result.subject;
                                from_jwt = true;
                            } else {
                                osw::log::Warn(kSubsystem,
                                               "JWT verify failed for rpc=%s reason=%s",
                                               rpc_method_.c_str(),
                                               result.error.c_str());
                                // Keep identity = "anonymous"; let RBAC decide.
                            }
                        }
                    }
                }
            }

            // --- Authorization ---
            auto decision = registry_->Authorize(identity, rpc_method_);

            if (decision.allowed) {
                osw::log::Debug(kSubsystem,
                                "authz allow identity=%s rpc=%s perm=%s",
                                identity.c_str(),
                                rpc_method_.c_str(),
                                decision.permission_required.c_str());
                osw::audit::Emit(kAuthzAllow, {
                    {"identity",            identity},
                    {"rpc",                 rpc_method_},
                    {"permission_required", decision.permission_required},
                    {"outcome",             "allow"},
                    {"identity_source",     from_jwt ? "jwt" : "mtls_cn"},
                });
                methods->Proceed();
                return;
            }

            // Denied path.
            osw::log::Warn(kSubsystem,
                           "authz deny identity=%s rpc=%s perm=%s reason=%s",
                           identity.c_str(),
                           rpc_method_.c_str(),
                           decision.permission_required.c_str(),
                           decision.deny_reason.c_str());
            osw::audit::Emit(kAuthzDeny, {
                {"identity",            identity},
                {"rpc",                 rpc_method_},
                {"permission_required", decision.permission_required},
                {"outcome",             "deny:" + decision.deny_reason},
                {"identity_source",     from_jwt ? "jwt" : "mtls_cn"},
            });

            // Hijack the RPC — we will synthesize the error response.
            denied_ = true;
            deny_anonymous_ = (decision.deny_reason == "unauthenticated");
            methods->Hijack();
            return;
        }

        if (methods->QueryInterceptionHookPoint(Hook::PRE_SEND_INITIAL_METADATA)) {
            // When hijacked, we must send the error.  Signal the ServerContext
            // to cancel with the appropriate status.
            if (denied_ && server_ctx_) {
                server_ctx_->TryCancel();
            }
            methods->Proceed();
            return;
        }

        if (methods->QueryInterceptionHookPoint(Hook::PRE_SEND_STATUS)) {
            // Override the outgoing status with the auth error.
            if (denied_) {
                grpc::Status err =
                    deny_anonymous_
                        ? grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                       "Authentication required")
                        : grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                       "Permission denied");
                methods->GetSendStatus() = err;
            }
            methods->Proceed();
            return;
        }

        // All other hooks: pass through.
        methods->Proceed();
    }

    /// Called by the factory right after construction, before any Intercept
    /// call, to inject the ServerContext pointer.  Thread-safe: called on the
    /// same gRPC worker thread that will call Intercept().
    void SetServerContext(grpc::ServerContextBase* ctx) noexcept {
        server_ctx_ = ctx;
    }

  private:
    std::shared_ptr<RbacRegistry> registry_;
    std::shared_ptr<JwtVerifier>  jwt_verifier_;
    std::string                   rpc_method_;
    grpc::ServerContextBase*      server_ctx_ = nullptr;
    bool                          denied_         = false;
    bool                          deny_anonymous_ = false;
};

// ---------------------------------------------------------------------------
// AuthInterceptorFactory
// ---------------------------------------------------------------------------

AuthInterceptorFactory::AuthInterceptorFactory(
    std::shared_ptr<RbacRegistry> registry,
    std::unique_ptr<JwtVerifier>  jwt_verifier) noexcept {
    registry_.store(std::move(registry), std::memory_order_release);
    // Wrap the unique_ptr in a shared_ptr for atomic storage.
    if (jwt_verifier) {
        jwt_verifier_.store(std::shared_ptr<JwtVerifier>(std::move(jwt_verifier)),
                            std::memory_order_release);
    }
}

grpc::experimental::Interceptor*
AuthInterceptorFactory::CreateServerInterceptor(
    grpc::experimental::ServerRpcInfo* info) {
    // Snapshot current registry and verifier atomically.
    auto registry     = registry_.load(std::memory_order_acquire);
    auto jwt_verifier = jwt_verifier_.load(std::memory_order_acquire);

    // Extract the RPC method name from ServerRpcInfo.
    std::string rpc_method;
    if (info && info->method()) {
        rpc_method = info->method();
    }

    auto* interceptor = new AuthInterceptor(
        std::move(registry),
        std::move(jwt_verifier),
        std::move(rpc_method));

    // Inject the ServerContext.  grpc::experimental::ServerRpcInfo exposes
    // server_context() in gRPC 1.74.
    if (info) {
        interceptor->SetServerContext(info->server_context());
    }

    return interceptor;
}

void AuthInterceptorFactory::UpdateRegistry(
    std::shared_ptr<RbacRegistry> new_registry) noexcept {
    registry_.store(std::move(new_registry), std::memory_order_release);
    osw::log::Info(kSubsystem, "RBAC registry reloaded");
}

void AuthInterceptorFactory::UpdateJwtVerifier(
    std::unique_ptr<JwtVerifier> new_verifier) noexcept {
    jwt_verifier_.store(std::shared_ptr<JwtVerifier>(std::move(new_verifier)),
                        std::memory_order_release);
    osw::log::Info(kSubsystem, "JWT verifier reloaded");
}

}  // namespace osw::control
