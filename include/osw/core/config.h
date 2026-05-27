/*
 * include/osw/core/config.h
 *
 * osw::Config — module configuration data + loader.
 *
 * W1 ships:
 *   - The Config data struct itself (all fields with default values).
 *   - Validate() — pure-C++ validation logic. No FS dependency.
 *
 * The loader function `LoadFromFile()` is declared in config_fs.h
 * (separate header, only included by code that has <switch.h>),
 * because it calls switch_xml_config_parse_module_settings (FF-013).
 * Tests of validation logic do NOT need to link the FS-dependent
 * loader.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CORE_CONFIG_H_
#define OSW_CORE_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace osw {

/// Validation result: success carries no detail; failure carries a
/// human-readable message suitable for the module load log line +
/// the Tier-1 audit event.
struct ConfigValidation {
    bool ok = true;
    std::string error;

    static ConfigValidation Ok() { return {}; }
    static ConfigValidation Fail(std::string msg) {
        ConfigValidation v;
        v.ok = false;
        v.error = std::move(msg);
        return v;
    }
};

/// Per-module configuration.
///
/// Defaults match the values documented in
/// deploy/freeswitch/conf/autoload_configs/open_switch.conf.xml
/// (landed in a later W1 commit). Operators set values via
/// `<param name="..." value="..."/>` elements; missing params keep
/// the default.
///
/// All field names are stable; tag-numbered to make adding new
/// params at the end backward-compatible (the XML loader doesn't
/// depend on order, but stable names matter for ops).
struct Config {
    // --- gRPC server ----------------------------------------------------
    /// Listen address. Default 0.0.0.0:50061.
    std::string grpc_listen_address = "0.0.0.0:50061";

    /// Max concurrent streams across SubscribeEvents + Media. 0 = unlimited.
    std::uint32_t grpc_max_concurrent_streams = 256;

    /// gRPC shutdown deadline (seconds). Drain() calls
    /// grpc::Server::Shutdown(now + grpc_drain_deadline_seconds).
    std::uint32_t grpc_drain_deadline_seconds = 2;

    // --- TLS (optional) -------------------------------------------------
    /// Path to PEM-encoded server certificate. Empty disables TLS.
    std::string grpc_tls_cert_path;
    /// Path to PEM-encoded server private key. Required if cert set.
    std::string grpc_tls_key_path;
    /// Path to PEM-encoded CA bundle for client-cert verification.
    /// Empty leaves mTLS off.
    std::string grpc_tls_ca_path;
    /// When true, client certificate is required and verified against
    /// grpc_tls_ca_path (mTLS). Automatically inferred as true when
    /// grpc_tls_ca_path is set, but operators can set this explicitly
    /// to override the heuristic. OQ-1 resolution: mTLS-CN preferred.
    bool grpc_tls_require_client_cert = false;

    // --- Event plane (W2 owns; defaults stored here for the schema)
    std::uint32_t event_ring_capacity_tier1 = 16384;
    std::uint32_t event_ring_capacity_tier2 = 8192;
    std::uint32_t event_ring_capacity_tier3 = 4096;
    /// Per-subscriber send queue capacity (events).
    std::uint32_t subscriber_send_queue_capacity = 4096;
    /// Max active SubscribeEvents streams.
    std::uint32_t max_subscribers = 16;

    // --- Idempotency (W3 owns; defaults stored here for the schema)
    std::uint32_t idempotency_ttl_seconds = 300;
    std::uint32_t idempotency_cache_capacity = 1500;
    std::uint32_t idempotency_in_flight_max_wait_seconds = 30;

    // --- Drain (W1 lifecycle observes the flag; full drain logic lands
    //           in W3/W4 alongside the owning subsystems)
    std::uint32_t drain_timeout_seconds = 30;
    // W2 owns this; W1 only stores the config value.
    std::uint32_t event_drain_timeout_seconds = 5;

    // --- Terminate handler (Codex round-3 finding N5)
    /// If true, install a process-wide std::set_terminate handler at
    /// module load that logs via signal-safe write() then chains the
    /// previous handler. Default OFF — see architecture.md §"Terminate
    /// handler chaining".
    bool osw_panic_on_unhandled = false;
    /// If true, install signal handlers for SIGSEGV/SIGABRT/SIGBUS at
    /// module load. Default OFF — FS installs its own handlers.
    bool osw_install_signal_handlers = false;

    // --- PII redaction patterns -----------------------------------------
    /// Regex patterns applied to log lines before emission. Empty list
    /// disables redaction.
    std::vector<std::string> pii_redaction_patterns;

    // --- Tenant ACL (W3 owns; defaults stored here for the schema)
    /// Per-tenant allowed dialplan contexts. Empty = deny-all in W3.
    /// Stored as "tenant:context1,context2;tenant:context3" for the
    /// XML-loader convenience (parsed into a map by the W3 ACL code).
    std::string tenant_allowed_contexts;

    // --- Observability --------------------------------------------------
    /// Log level threshold ("trace","debug","info","warn","error","crit").
    std::string log_level = "info";

    // --- Metrics (W4 Track C) -------------------------------------------
    /// Enable the Prometheus /metrics HTTP endpoint.
    bool metrics_enabled = true;
    /// Bind address for the metrics HTTP server.
    /// Default: 127.0.0.1 (loopback only). Operators expose via reverse proxy.
    std::string metrics_bind_address = "127.0.0.1";
    /// TCP port for the metrics HTTP server.
    std::uint16_t metrics_port = 9090;

    // --- Media (W6 Track C) ---------------------------------------------
    /// TTS playout jitter buffer target depth. Range: 200-5000 ms;
    /// Validate() clamps. Default 1000.
    std::uint32_t tts_jitter_buffer_ms = 1000;

    /// Pre-roll: the playback waits until the buffer accumulates at
    /// least this many ms before emitting the first non-silence frame.
    /// Validate() clamps to [50, tts_jitter_buffer_ms]. Default 500.
    std::uint32_t tts_preroll_ms = 500;

    /// High-water: when buffer depth exceeds this, the producer drops
    /// the OLDEST queued frame on each Push. Validate() clamps to
    /// [tts_jitter_buffer_ms, tts_max_jitter_buffer_ms]. Default 1500.
    std::uint32_t tts_high_water_ms = 1500;

    /// Hard cap on per-call jitter buffer override. Validate() clamps
    /// tts_jitter_buffer_ms to <= this and rejects per-call overrides
    /// above this. Default 5000.
    std::uint32_t tts_max_jitter_buffer_ms = 5000;

    /// Underrun policy: "silence" (default - clean for speech) or
    /// "repeat_last" (copies last 20 ms frame; better for music).
    std::string tts_underrun_policy = "silence";

    // --- Media (W6.6) ----------------------------------------------------
    /// Enable module-owned silence_stream://-1 driver threads for parked
    /// WRITE_REPLACE channels that have no other write-side source.
    bool silence_driver_enabled = true;

    /// Hard cap on simultaneous silence driver threads.
    std::uint32_t max_silence_drivers = 200;

    // --- Bot media facade (W7 Track D) -----------------------------------
    /// Max number of target channels per StartBot call. V1 demo scope is 2.
    std::uint32_t bot_max_targets = 2;
    /// Per-target fanout queue capacity in milliseconds. Used by the W7
    /// BugFanout path; retained in config while StartBot facade lands.
    std::uint32_t bot_target_queue_ms = 500;
    /// StopBot drain timeout in milliseconds.
    std::uint32_t bot_drain_timeout_ms = 2000;
    /// Hard cap on simultaneous logical bots per channel.
    std::uint32_t max_bots_per_channel = 1;
};

/// Validates the config. Returns Ok() or Fail(detail).
///
/// Rules:
///   - grpc_listen_address must be non-empty and contain ':'.
///   - grpc_tls_key_path required iff grpc_tls_cert_path set.
///   - grpc_tls_ca_path optional; if set, grpc_tls_cert_path must
///     also be set.
///   - event_ring_capacity_tier{1,2,3} >= 256.
///   - subscriber_send_queue_capacity >= 64.
///   - max_subscribers >= 1.
///   - idempotency_ttl_seconds > 0.
///   - idempotency_cache_capacity >= 16.
///   - drain_timeout_seconds > 0.
///   - log_level one of {trace,debug,info,warn,error,crit}.
///   - Each pattern in pii_redaction_patterns must be a valid std::regex.
ConfigValidation Validate(const Config& config);

}  // namespace osw

#endif  // OSW_CORE_CONFIG_H_
