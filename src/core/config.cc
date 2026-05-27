/*
 * src/core/config.cc — Validation logic for osw::Config.
 *
 * No FreeSWITCH dependency; testable without an FS process.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/config.h"

#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace osw {

ConfigValidation Validate(const Config& cfg) {
    // --- gRPC listen address ----------------------------------------------
    if (cfg.grpc_listen_address.empty()) {
        return ConfigValidation::Fail("grpc_listen_address must be set");
    }
    if (cfg.grpc_listen_address.find(':') == std::string::npos) {
        return ConfigValidation::Fail(
            "grpc_listen_address must contain a ':' (e.g. 0.0.0.0:50061)");
    }

    // --- TLS coherency ----------------------------------------------------
    if (!cfg.grpc_tls_cert_path.empty() && cfg.grpc_tls_key_path.empty()) {
        return ConfigValidation::Fail(
            "grpc_tls_cert_path set but grpc_tls_key_path is empty — both required for TLS");
    }
    if (cfg.grpc_tls_key_path.empty() && !cfg.grpc_tls_ca_path.empty()) {
        return ConfigValidation::Fail(
            "grpc_tls_ca_path set without grpc_tls_cert_path — mTLS requires server cert + key");
    }
    if (cfg.grpc_tls_require_client_cert && cfg.grpc_tls_cert_path.empty()) {
        return ConfigValidation::Fail(
            "grpc_tls_require_client_cert=true but grpc_tls_cert_path is empty — "
            "mTLS requires server cert + key");
    }

    // --- Ring capacities --------------------------------------------------
    constexpr std::uint32_t kMinRingCapacity = 256;
    if (cfg.event_ring_capacity_tier1 < kMinRingCapacity) {
        return ConfigValidation::Fail("event_ring_capacity_tier1 must be >= 256");
    }
    if (cfg.event_ring_capacity_tier2 < kMinRingCapacity) {
        return ConfigValidation::Fail("event_ring_capacity_tier2 must be >= 256");
    }
    if (cfg.event_ring_capacity_tier3 < kMinRingCapacity) {
        return ConfigValidation::Fail("event_ring_capacity_tier3 must be >= 256");
    }

    if (cfg.subscriber_send_queue_capacity < 64) {
        return ConfigValidation::Fail("subscriber_send_queue_capacity must be >= 64");
    }
    if (cfg.max_subscribers < 1) {
        return ConfigValidation::Fail("max_subscribers must be >= 1");
    }

    // --- Idempotency ------------------------------------------------------
    if (cfg.idempotency_ttl_seconds == 0) {
        return ConfigValidation::Fail("idempotency_ttl_seconds must be > 0");
    }
    if (cfg.idempotency_cache_capacity < 16) {
        return ConfigValidation::Fail("idempotency_cache_capacity must be >= 16");
    }
    if (cfg.idempotency_in_flight_max_wait_seconds == 0) {
        return ConfigValidation::Fail("idempotency_in_flight_max_wait_seconds must be > 0");
    }

    // --- Drain ------------------------------------------------------------
    if (cfg.drain_timeout_seconds == 0) {
        return ConfigValidation::Fail("drain_timeout_seconds must be > 0");
    }
    if (cfg.grpc_drain_deadline_seconds == 0) {
        return ConfigValidation::Fail("grpc_drain_deadline_seconds must be > 0");
    }

    // --- Log level --------------------------------------------------------
    static const std::set<std::string> kValidLevels{
        "trace", "debug", "info", "warn", "error", "crit"};
    if (!kValidLevels.contains(cfg.log_level)) {
        std::ostringstream oss;
        oss << "log_level must be one of trace/debug/info/warn/error/crit; got: " << cfg.log_level;
        return ConfigValidation::Fail(oss.str());
    }

    // --- PII redaction patterns -----------------------------------------
    // Try to compile each one to catch syntax errors at load time. The
    // compiled regexes are discarded here (config.h carries strings;
    // the log subsystem compiles them when called).
    for (std::size_t i = 0; i < cfg.pii_redaction_patterns.size(); ++i) {
        try {
            (void)std::regex(cfg.pii_redaction_patterns[i]);
        } catch (const std::regex_error& e) {
            std::ostringstream oss;
            oss << "pii_redaction_patterns[" << i << "] is not a valid regex: " << e.what();
            return ConfigValidation::Fail(oss.str());
        }
    }

    return ConfigValidation::Ok();
}

}  // namespace osw
