/*
 * include/osw/control/config_tls.h
 *
 * Helper to build osw::control::TlsConfig from osw::Config.
 *
 * This header is FS-agnostic (no <switch.h> dependency) — it only
 * needs osw::Config and osw::control::TlsConfig, both of which are
 * plain C++ structs. Tests can include this directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_CONFIG_TLS_H_
#define OSW_CONTROL_CONFIG_TLS_H_

#include "osw/control/tls_config.h"
#include "osw/core/config.h"

namespace osw::control {

/// Converts the flat TLS fields in `config` (grpc_tls_cert_path,
/// grpc_tls_key_path, grpc_tls_ca_path, grpc_tls_require_client_cert)
/// into a TlsConfig.
///
/// Decision OQ-1: require_client_cert is set when
/// config.grpc_tls_require_client_cert is true OR
/// config.grpc_tls_ca_path is non-empty (implicit mTLS intent).
TlsConfig TlsConfigFromConfig(const osw::Config& config);

}  // namespace osw::control

#endif  // OSW_CONTROL_CONFIG_TLS_H_
