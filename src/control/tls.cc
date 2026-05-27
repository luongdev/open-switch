/*
 * src/control/tls.cc — BuildServerCredentials + MakeServerCreds.
 *
 * W4 Track A replaces the W1 stub with real grpc::SslServerCredentials
 * construction backed by PEM files read from disk (FF-028).
 *
 * Design decisions inlined here (pre-decided before W4 implementation):
 *
 *   OQ-1 (mTLS-CN preferred, JWT fallback):
 *     When TlsConfig::require_client_cert is true and ca_path is set,
 *     the server enforces full mTLS:
 *       client_certificate_request =
 *           REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY  (value 4)
 *     Clients that cannot present a cert fall back to JWT Bearer token
 *     identity (handled by the W4 Track B auth interceptor, not here).
 *
 *   OQ-2 (default-deny):
 *     If TLS is configured (cert_path non-empty) but any PEM file is
 *     missing or unreadable, BuildServerCredentials returns nullptr.
 *     GrpcServer::Start treats nullptr as a hard load failure — the
 *     module will not start with a broken TLS configuration.
 *     InsecureServerCredentials is only returned when cert_path is
 *     explicitly empty (dev / loopback mode).
 *
 *   OQ-5 (SIGHUP via inotify, not signal()):
 *     This TU does not install any POSIX signal handlers. Cert hot-reload
 *     is driven by TlsReloader (src/control/tls_reloader.cc) which calls
 *     BuildServerCredentials again whenever inotify detects a cert-file
 *     change (FF-030).
 *
 * Logger subsystem: "control.tls"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/tls.h"

#include <fstream>
#include <sstream>
#include <string>

#include <grpcpp/security/server_credentials.h>

#include "osw/control/tls_config.h"
#include "osw/core/config.h"
#include "osw/observability/log.h"

namespace osw::control {

namespace {

// Slurps an entire PEM file into a std::string. Returns empty string on
// any I/O error so callers can check `result.empty()`.
std::string SlurpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Returns true iff the string looks like it contains at least one PEM
// block. A valid PEM file always starts (after optional whitespace) with
// "-----BEGIN". This is a lightweight sanity check that catches truncated
// or plaintext-path mistakes before gRPC rejects them with a cryptic error.
bool LooksLikePem(const std::string& s) {
    return s.find("-----BEGIN") != std::string::npos;
}

}  // namespace

// Primary W4 API — reads PEM files from disk and builds
// grpc::SslServerCredentials.
//
// Return value contract (FF-028):
//   nullptr  → caller must treat this as a fatal error.
//   non-null → grpc::InsecureServerCredentials() OR grpc::SslServerCredentials().
std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(const TlsConfig& cfg) {
    // --- Dev / loopback mode -----------------------------------------------
    if (!cfg.enabled()) {
        osw::log::Info("control.tls",
                       "TLS not configured (cert_path empty); "
                       "starting with InsecureServerCredentials — "
                       "NOT suitable for production");
        return grpc::InsecureServerCredentials();
    }

    // --- Read server certificate -------------------------------------------
    const std::string cert = SlurpFile(cfg.cert_path);
    if (cert.empty()) {
        osw::log::Error("control.tls", "failed to read TLS cert at %s", cfg.cert_path.c_str());
        return {};
    }
    if (!LooksLikePem(cert)) {
        osw::log::Error("control.tls",
                        "TLS cert at %s does not appear to be PEM-encoded (no '-----BEGIN' marker)",
                        cfg.cert_path.c_str());
        return {};
    }

    // --- Read server private key -------------------------------------------
    const std::string key = SlurpFile(cfg.key_path);
    if (key.empty()) {
        osw::log::Error("control.tls", "failed to read TLS key at %s", cfg.key_path.c_str());
        return {};
    }
    if (!LooksLikePem(key)) {
        osw::log::Error("control.tls",
                        "TLS key at %s does not appear to be PEM-encoded (no '-----BEGIN' marker)",
                        cfg.key_path.c_str());
        return {};
    }

    // --- Build SslServerCredentialsOptions (FF-028) ------------------------
    //
    // Decision OQ-1: mTLS-CN preferred identity source.
    // When require_client_cert is true, set
    //   GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY (4).
    // Otherwise set GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE (0).
    //
    // gRPC 1.74 exposes the request type via the C enum
    // `grpc_ssl_client_certificate_request_type` declared in
    // <grpc/grpc_security_constants.h>. The deprecated nested
    // `SslServerCredentialsOptions::ClientCertificateRequestType`
    // referenced by an earlier draft of this file no longer exists
    // in the gRPC 1.74 ABI we ship.
    const grpc_ssl_client_certificate_request_type cert_req =
        cfg.require_client_cert ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
                                : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;

    grpc::SslServerCredentialsOptions opts(cert_req);

    // --- Read CA bundle for mTLS -------------------------------------------
    if (!cfg.ca_path.empty()) {
        const std::string ca = SlurpFile(cfg.ca_path);
        if (ca.empty()) {
            osw::log::Error(
                "control.tls", "failed to read TLS CA bundle at %s", cfg.ca_path.c_str());
            return {};
        }
        if (!LooksLikePem(ca)) {
            osw::log::Error("control.tls",
                            "TLS CA at %s does not appear to be PEM-encoded",
                            cfg.ca_path.c_str());
            return {};
        }
        opts.pem_root_certs = ca;
    }

    // grpc::SslServerCredentialsOptions::PemKeyCertPair: { private_key, cert_chain }
    opts.pem_key_cert_pairs.push_back({key, cert});

    osw::log::Info("control.tls",
                   "TLS enabled — cert=%s key=%s ca=%s mtls=%s",
                   cfg.cert_path.c_str(),
                   cfg.key_path.c_str(),
                   cfg.ca_path.empty() ? "(none)" : cfg.ca_path.c_str(),
                   cfg.require_client_cert ? "yes" : "no");

    return grpc::SslServerCredentials(opts);
}

// Legacy W1 compatibility wrapper. Synthesises a TlsConfig from the
// flat fields in osw::Config and delegates to BuildServerCredentials.
std::shared_ptr<grpc::ServerCredentials> MakeServerCreds(const Config& config) {
    TlsConfig cfg;
    cfg.cert_path = config.grpc_tls_cert_path;
    cfg.key_path = config.grpc_tls_key_path;
    cfg.ca_path = config.grpc_tls_ca_path;
    // Use the explicit flag if set, otherwise infer from ca_path presence.
    // OQ-1: mTLS-CN preferred; only enabled when operator provides CA bundle.
    cfg.require_client_cert =
        config.grpc_tls_require_client_cert || !config.grpc_tls_ca_path.empty();
    return BuildServerCredentials(cfg);
}

}  // namespace osw::control
