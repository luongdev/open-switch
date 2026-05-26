/*
 * src/control/tls.cc — MakeServerCreds implementation.
 *
 * Reads PEM files from disk via std::ifstream (RAII).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/tls.h"

#include <fstream>
#include <sstream>
#include <string>

#include <grpcpp/security/server_credentials.h>

#include "osw/core/config.h"
#include "osw/observability/log.h"

namespace osw::control {

namespace {

std::string SlurpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

}  // namespace

std::shared_ptr<grpc::ServerCredentials> MakeServerCreds(const Config& config) {
    if (config.grpc_tls_cert_path.empty()) {
        osw::log::Info("control",
                       "gRPC TLS not configured (no grpc_tls_cert_path); "
                       "starting with insecure credentials");
        return grpc::InsecureServerCredentials();
    }

    const std::string cert = SlurpFile(config.grpc_tls_cert_path);
    if (cert.empty()) {
        osw::log::Error(
            "control", "failed to read TLS cert at %s", config.grpc_tls_cert_path.c_str());
        return {};
    }
    const std::string key = SlurpFile(config.grpc_tls_key_path);
    if (key.empty()) {
        osw::log::Error(
            "control", "failed to read TLS key at %s", config.grpc_tls_key_path.c_str());
        return {};
    }

    grpc::SslServerCredentialsOptions opts(
        config.grpc_tls_ca_path.empty()
            ? GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE
            : GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);

    if (!config.grpc_tls_ca_path.empty()) {
        const std::string ca = SlurpFile(config.grpc_tls_ca_path);
        if (ca.empty()) {
            osw::log::Error(
                "control", "failed to read TLS CA bundle at %s", config.grpc_tls_ca_path.c_str());
            return {};
        }
        opts.pem_root_certs = ca;
    }

    opts.pem_key_cert_pairs.push_back({key, cert});

    osw::log::Info("control",
                   "gRPC TLS enabled (cert=%s%s)",
                   config.grpc_tls_cert_path.c_str(),
                   config.grpc_tls_ca_path.empty() ? "" : "; mTLS on");
    return grpc::SslServerCredentials(opts);
}

}  // namespace osw::control
