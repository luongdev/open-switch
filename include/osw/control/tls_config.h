/*
 * include/osw/control/tls_config.h — TlsConfig struct for W4 Track A.
 *
 * Holds the four parameters that fully describe the TLS / mTLS posture
 * for the gRPC server:
 *
 *   cert_path          — PEM-encoded server certificate file.
 *   key_path           — PEM-encoded server private key file.
 *   ca_path            — PEM-encoded CA bundle for client-cert verification.
 *                        Empty = TLS-only (no client cert required).
 *   require_client_cert — When true AND ca_path is set, enforce mTLS:
 *                         grpc::SslServerCredentialsOptions value
 *                         REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
 *                         (FF-028). When false, use
 *                         DO_NOT_REQUEST_CLIENT_CERTIFICATE.
 *
 * Design decisions (OQ-1, OQ-5 resolution — pre-decided before W4):
 *   - Default-deny when no auth configured (OQ-2): mTLS is the preferred
 *     identity path. If TLS is disabled entirely (cert_path empty), the
 *     gRPC server falls back to insecure credentials and all RPCs are
 *     unauthenticated — only suitable for loopback / dev.
 *   - mTLS-CN preferred, JWT fallback (OQ-1): when mTLS is enabled and
 *     a valid client cert is presented, the CN is used as identity.
 *     JWT Bearer tokens are the fallback for clients that cannot present
 *     a cert (Track B — W4 Auth interceptor).
 *   - SIGHUP via inotify (OQ-5): FS does not re-invoke the module's
 *     load() on SIGHUP (FF-030). Cert reload is handled by TlsReloader
 *     (src/control/tls_reloader.cc) watching the cert directory with
 *     inotify. No POSIX signal() handler is installed.
 *
 * Relationship to osw::Config:
 *   The main osw::Config struct (include/osw/core/config.h) carries the
 *   three flat path strings (grpc_tls_cert_path, grpc_tls_key_path,
 *   grpc_tls_ca_path). TlsConfig is a richer view: it adds
 *   require_client_cert (derived from whether ca_path is set, plus a
 *   future explicit config key) and is the type accepted by
 *   BuildServerCredentials() and TlsReloader.
 *
 * Logger subsystem: "control.tls"
 * Audit: "osw.control.tls.reload" on each successful credential rebuild.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_TLS_CONFIG_H_
#define OSW_CONTROL_TLS_CONFIG_H_

#include <string>

namespace osw::control {

/// Configuration for the gRPC server's TLS / mTLS posture.
///
/// Populated by config_tls.cc from the <tls> block in
/// open_switch.conf.xml (or synthesised from the legacy flat params in
/// osw::Config for backward compatibility).
struct TlsConfig {
    /// Path to the PEM-encoded server certificate. Required when TLS is
    /// enabled. Empty = TLS disabled (InsecureServerCredentials).
    std::string cert_path;

    /// Path to the PEM-encoded server private key.  Required when
    /// cert_path is set.
    std::string key_path;

    /// Path to the PEM-encoded CA bundle used to verify client
    /// certificates (mTLS). Empty = no client-cert verification;
    /// TLS-only mode.
    std::string ca_path;

    /// When true, client certificate presentation AND verification is
    /// required (grpc::SslServerCredentialsOptions value 4, FF-028).
    /// When false, the server does not request a client certificate
    /// (value 0).
    ///
    /// This field is automatically set to `true` by FromConfig() when
    /// ca_path is non-empty, because a CA bundle only makes sense when
    /// client certs are being verified. Operators who want the CA bundle
    /// available but only optional client certs must set
    /// require_client_cert = false explicitly.
    bool require_client_cert = false;

    /// Returns true iff TLS is configured (cert_path non-empty).
    [[nodiscard]] bool enabled() const noexcept { return !cert_path.empty(); }
};

}  // namespace osw::control

#endif  // OSW_CONTROL_TLS_CONFIG_H_
