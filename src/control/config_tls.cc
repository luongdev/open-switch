/*
 * src/core/config_tls.cc — TlsConfig builder from osw::Config.
 *
 * Converts the flat TLS params in osw::Config (loaded from the
 * <settings> block in open_switch.conf.xml) into an osw::control::TlsConfig
 * suitable for passing to BuildServerCredentials and TlsReloader.
 *
 * The <tls> block design note:
 *   W4 adds a dedicated <tls> section to open_switch.conf.xml (see the
 *   conf.xml sample). The new section adds the boolean param
 *   "grpc_tls_require_client_cert" which lets operators explicitly control
 *   mTLS without relying on the "ca_path non-empty → require client cert"
 *   heuristic. The three path params are also duplicated under <tls> for
 *   readability; the legacy flat params in <settings> continue to work for
 *   backward compatibility (config_fs.cc parses both).
 *
 * Decision OQ-1 (mTLS-CN preferred):
 *   require_client_cert is set to true when:
 *     (a) config.grpc_tls_require_client_cert is explicitly true, OR
 *     (b) config.grpc_tls_ca_path is non-empty (implicit mTLS intent).
 *   This means an operator who sets only a CA path gets mTLS by default
 *   without having to add a separate boolean param.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/config_tls.h"

#include "osw/control/tls_config.h"
#include "osw/core/config.h"

namespace osw::control {

/// Build a TlsConfig from the flat TLS params in osw::Config.
///
/// Called by GrpcServer::Start (via MakeServerCreds) and by Module::Load
/// when setting up TlsReloader. For new code, prefer to pass TlsConfig
/// directly; this function is the bridge for the legacy Config path.
TlsConfig TlsConfigFromConfig(const osw::Config& config) {
    TlsConfig cfg;
    cfg.cert_path = config.grpc_tls_cert_path;
    cfg.key_path = config.grpc_tls_key_path;
    cfg.ca_path = config.grpc_tls_ca_path;
    // OQ-1 resolution: mTLS-CN preferred. Enable require_client_cert when:
    //   - operator set it explicitly (grpc_tls_require_client_cert=true), OR
    //   - operator provided a CA bundle (implicit mTLS intent).
    cfg.require_client_cert =
        config.grpc_tls_require_client_cert || !config.grpc_tls_ca_path.empty();
    return cfg;
}

}  // namespace osw::control
