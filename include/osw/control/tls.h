/*
 * include/osw/control/tls.h — gRPC server credential builders.
 *
 * W4 Track A replaces the W1 stub with a real implementation backed
 * by grpc::SslServerCredentials (FF-028). Two entry points are exposed:
 *
 *   BuildServerCredentials(TlsConfig)
 *     Primary W4 API. Reads cert/key/CA PEM from disk via std::ifstream.
 *     Returns null on any read or validation error so the caller can
 *     treat it as a fatal configuration problem.
 *
 *   MakeServerCreds(Config)
 *     Legacy W1 wrapper kept for backward compatibility with GrpcServer::
 *     Start(). Internally constructs a TlsConfig from the flat Config
 *     fields and delegates to BuildServerCredentials.
 *
 * Design decisions documented here (OQ-1 / OQ-5):
 *   - mTLS-CN preferred identity (OQ-1): when require_client_cert is
 *     true the server uses REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
 *     (FF-028 enum value 4). JWT is the fallback when no client cert
 *     is presented (Track B interceptor).
 *   - Default-deny (OQ-2): if TLS is configured but PEM files are
 *     missing or unreadable, BuildServerCredentials returns nullptr.
 *     GrpcServer::Start treats nullptr as a fatal load error. Only
 *     when cert_path is explicitly empty does the function return
 *     InsecureServerCredentials (dev/loopback mode).
 *
 * Logger subsystem: "control.tls"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_TLS_H_
#define OSW_CONTROL_TLS_H_

#include <memory>
#include <string>

#include <grpcpp/security/server_credentials.h>

#include "osw/control/tls_config.h"

namespace osw {
struct Config;

namespace control {

/// Reads PEM strings from the paths in `cfg` and builds gRPC server
/// credentials. Decision tree:
///
///   - cfg.cert_path empty → InsecureServerCredentials() (dev mode).
///     Logs an INFO message; never returns null in this branch.
///   - cert_path set but file unreadable → nullptr (fatal).
///   - key_path unreadable → nullptr (fatal).
///   - ca_path set but unreadable → nullptr (fatal).
///   - cfg.require_client_cert true (and ca_path set) →
///     SslServerCredentialsOptions with
///     REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY (FF-028, value 4).
///   - otherwise → DO_NOT_REQUEST_CLIENT_CERTIFICATE (FF-028, value 0).
///
/// Returns a valid shared_ptr on success, nullptr on any error. The
/// returned credentials can be passed directly to
/// grpc::ServerBuilder::AddListeningPort.
std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(const TlsConfig& cfg);

/// Legacy W1 compatibility wrapper. Constructs a TlsConfig from the
/// flat grpc_tls_* fields in `config` (where require_client_cert is
/// inferred as true when grpc_tls_ca_path is non-empty) and delegates
/// to BuildServerCredentials.
///
/// Kept so GrpcServer::Start and existing tests do not need changes.
/// New code should call BuildServerCredentials(TlsConfig) directly.
std::shared_ptr<grpc::ServerCredentials> MakeServerCreds(const Config& config);

}  // namespace control
}  // namespace osw

#endif  // OSW_CONTROL_TLS_H_
