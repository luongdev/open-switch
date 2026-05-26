/*
 * include/osw/control/tls.h
 *
 * Builds grpc::ServerCredentials from osw::Config.
 *
 * Per W1 contract §"src/control/tls.h":
 *   - If grpc_tls_cert_path + grpc_tls_key_path are set, build
 *     grpc::SslServerCredentials with optional mTLS client-cert
 *     verification via grpc_tls_ca_path.
 *   - Otherwise return grpc::InsecureServerCredentials().
 *
 * PEM files are loaded via std::ifstream — RAII covers cleanup.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_TLS_H_
#define OSW_CONTROL_TLS_H_

#include <memory>

#include <grpcpp/security/server_credentials.h>

namespace osw {
struct Config;

namespace control {

/// Builds server credentials from `config`. Never returns null:
/// either real TLS creds or InsecureServerCredentials().
///
/// If config requests TLS but the cert / key files cannot be read,
/// this returns null — the caller (GrpcServer::Start) treats that as
/// a load failure.
std::shared_ptr<grpc::ServerCredentials> MakeServerCreds(const Config& config);

}  // namespace control
}  // namespace osw

#endif  // OSW_CONTROL_TLS_H_
