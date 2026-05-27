/*
 * src/core/config_fs.cc — FS-dependent half of the config loader.
 *
 * Calls switch_xml_config_parse_module_settings (FF-013) to read
 * <param name="..." value="..."/> elements out of the module's XML
 * config file. Returns true if the file was either successfully
 * parsed OR absent (defaults preserved); returns false only on a
 * real parse error.
 *
 * FACTs cited:
 *   - FF-013 — switch_xml_config_parse_module_settings semantics.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/config_fs.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <switch_xml_config.h>

#include <switch.h>  // FF-013

#include "osw/observability/log.h"

namespace osw {

namespace {

// Each string param needs a heap buffer FS can strdup into. Switch's
// SWITCH_CONFIG_STRING with switch_config_string_strdup heap-allocates
// via switch_safe_strdup (uses malloc); we then assign into the
// std::string and free the C-string. Wrapping the pair into a small
// owner here keeps the lifetime obvious.
struct StringParam {
    char* dest_cstr = nullptr;  // FS strdup writes here; we free on return.
    Config* config = nullptr;
    std::string* target = nullptr;
    const char* key = nullptr;
};

// We use a callback-based string param: when FS parses the value, it
// calls our callback which dups into the destination std::string.
switch_status_t StringCallback(switch_xml_config_item_t* item,
                               const char* newvalue,
                               switch_config_callback_type_t /*ct*/,
                               switch_bool_t /*changed*/) {
    auto* sp = static_cast<StringParam*>(item->data);
    if (!sp || !sp->target) {
        return SWITCH_STATUS_SUCCESS;
    }
    if (newvalue) {
        sp->target->assign(newvalue);
    } else if (item->defaultvalue) {
        sp->target->assign(static_cast<const char*>(item->defaultvalue));
    }
    return SWITCH_STATUS_SUCCESS;
}

// For repeated string params (PII regex list), we accept a single
// pipe-separated string and split it at parse time. Operators write:
//   <param name="pii_redaction_patterns" value="\+\d{8,15}|\bAC[A-Z]{3}\d{6}\b"/>
// (Pipes are escaped at the XML layer by buf-driven escaping; in
// practice operators use a wrapper string the W4.5 docs explain.)
//
// Contract: always-skip-empty for ALL positions (leading, intermediate,
// trailing). An empty regex would compile to "match empty string" —
// which redacts every log line to [REDACTED] — and is never what the
// operator wanted. Reviewed in codex-W1.md I-2.
void SplitPipeList(const std::string& src, std::vector<std::string>& out) {
    out.clear();
    if (src.empty()) {
        return;
    }
    std::size_t start = 0;
    while (start <= src.size()) {
        const std::size_t end = src.find('|', start);
        std::string chunk;
        if (end == std::string::npos) {
            chunk = src.substr(start);
            if (!chunk.empty()) {
                out.emplace_back(std::move(chunk));
            }
            return;
        }
        chunk = src.substr(start, end - start);
        if (!chunk.empty()) {
            out.emplace_back(std::move(chunk));
        }
        start = end + 1;
    }
}

}  // namespace

bool LoadConfigFromFile(const char* xml_file_name, Config& out) {
    // 1. Reset to defaults (handled by initial out = Config{} by the
    //    caller; assert here for clarity).
    out = Config{};

    // 2. Build the switch_xml_config_item_t table. Each row writes
    //    directly into `out` via a callback (for std::string fields)
    //    or via switch_xml_config_parse's built-in integer / bool
    //    handling (with default fall-through).
    //
    // We hold per-string-param state in StringParam structs.

    StringParam sp_listen{};
    sp_listen.target = &out.grpc_listen_address;
    StringParam sp_cert{};
    sp_cert.target = &out.grpc_tls_cert_path;
    StringParam sp_key{};
    sp_key.target = &out.grpc_tls_key_path;
    StringParam sp_ca{};
    sp_ca.target = &out.grpc_tls_ca_path;
    switch_bool_t bool_require_client_cert =
        out.grpc_tls_require_client_cert ? SWITCH_TRUE : SWITCH_FALSE;
    StringParam sp_log{};
    sp_log.target = &out.log_level;
    StringParam sp_acl{};
    sp_acl.target = &out.tenant_allowed_contexts;
    std::string pii_pipe;
    StringParam sp_pii{};
    sp_pii.target = &pii_pipe;

    // W6 Track C — TTS playout buffer params
    int int_tts_jitter = static_cast<int>(out.tts_jitter_buffer_ms);
    int int_tts_preroll = static_cast<int>(out.tts_preroll_ms);
    int int_tts_high_water = static_cast<int>(out.tts_high_water_ms);
    int int_tts_max_jitter = static_cast<int>(out.tts_max_jitter_buffer_ms);
    StringParam sp_tts_underrun{};
    sp_tts_underrun.target = &out.tts_underrun_policy;

    switch_xml_config_int_options_t opt_ge200{};
    opt_ge200.enforce_min = SWITCH_TRUE;
    opt_ge200.min = 200;
    switch_xml_config_int_options_t opt_ge50{};
    opt_ge50.enforce_min = SWITCH_TRUE;
    opt_ge50.min = 50;

    // The switch_xml_config_int_options_t is a stack struct that lives
    // for the duration of the parse; the table holds a pointer into it.
    switch_xml_config_int_options_t opt_ge1{};
    opt_ge1.enforce_min = SWITCH_TRUE;
    opt_ge1.min = 1;
    switch_xml_config_int_options_t opt_ge16{};
    opt_ge16.enforce_min = SWITCH_TRUE;
    opt_ge16.min = 16;
    switch_xml_config_int_options_t opt_ge64{};
    opt_ge64.enforce_min = SWITCH_TRUE;
    opt_ge64.min = 64;
    switch_xml_config_int_options_t opt_ge256{};
    opt_ge256.enforce_min = SWITCH_TRUE;
    opt_ge256.min = 256;

    // We keep int destinations in `int` slots and copy back into the
    // typed fields at the end. This avoids the type-punning warnings
    // that come from pointing FS's int handler at a std::uint32_t*.
    int int_max_streams = static_cast<int>(out.grpc_max_concurrent_streams);
    int int_drain_deadline = static_cast<int>(out.grpc_drain_deadline_seconds);
    int int_ring_t1 = static_cast<int>(out.event_ring_capacity_tier1);
    int int_ring_t2 = static_cast<int>(out.event_ring_capacity_tier2);
    int int_ring_t3 = static_cast<int>(out.event_ring_capacity_tier3);
    int int_sub_send = static_cast<int>(out.subscriber_send_queue_capacity);
    int int_max_sub = static_cast<int>(out.max_subscribers);
    int int_idemp_ttl = static_cast<int>(out.idempotency_ttl_seconds);
    int int_idemp_cap = static_cast<int>(out.idempotency_cache_capacity);
    int int_idemp_max_wait = static_cast<int>(out.idempotency_in_flight_max_wait_seconds);
    int int_drain_to = static_cast<int>(out.drain_timeout_seconds);
    int int_evdrain_to = static_cast<int>(out.event_drain_timeout_seconds);

    switch_bool_t bool_panic = out.osw_panic_on_unhandled ? SWITCH_TRUE : SWITCH_FALSE;
    switch_bool_t bool_sigh = out.osw_install_signal_handlers ? SWITCH_TRUE : SWITCH_FALSE;

    switch_xml_config_item_t instructions[] = {
        // gRPC server
        SWITCH_CONFIG_ITEM_CALLBACK("grpc_listen_address",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "0.0.0.0:50061",
                                    &StringCallback,
                                    &sp_listen,
                                    "host:port",
                                    "gRPC server listen address"),
        SWITCH_CONFIG_ITEM("grpc_max_concurrent_streams",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_max_streams,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(256)),
                           &opt_ge1,
                           "uint",
                           "Max concurrent gRPC streams"),
        SWITCH_CONFIG_ITEM("grpc_drain_deadline_seconds",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_drain_deadline,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(2)),
                           &opt_ge1,
                           "seconds",
                           "gRPC server shutdown deadline"),

        // TLS
        SWITCH_CONFIG_ITEM_CALLBACK("grpc_tls_cert_path",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "",
                                    &StringCallback,
                                    &sp_cert,
                                    "path",
                                    "PEM server cert (empty = TLS off)"),
        SWITCH_CONFIG_ITEM_CALLBACK("grpc_tls_key_path",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "",
                                    &StringCallback,
                                    &sp_key,
                                    "path",
                                    "PEM server private key"),
        SWITCH_CONFIG_ITEM_CALLBACK("grpc_tls_ca_path",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "",
                                    &StringCallback,
                                    &sp_ca,
                                    "path",
                                    "PEM CA bundle (empty = no mTLS)"),
        SWITCH_CONFIG_ITEM("grpc_tls_require_client_cert",
                           SWITCH_CONFIG_BOOL,
                           CONFIG_RELOADABLE,
                           &bool_require_client_cert,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(SWITCH_FALSE)),
                           nullptr,
                           "true|false",
                           "Require client cert (mTLS). Auto-true when ca_path set (OQ-1)"),

        // Event plane
        SWITCH_CONFIG_ITEM("event_ring_capacity_tier1",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_ring_t1,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(16384)),
                           &opt_ge256,
                           "events",
                           "Tier-1 ring capacity"),
        SWITCH_CONFIG_ITEM("event_ring_capacity_tier2",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_ring_t2,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(8192)),
                           &opt_ge256,
                           "events",
                           "Tier-2 ring capacity"),
        SWITCH_CONFIG_ITEM("event_ring_capacity_tier3",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_ring_t3,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(4096)),
                           &opt_ge256,
                           "events",
                           "Tier-3 ring capacity"),
        SWITCH_CONFIG_ITEM("subscriber_send_queue_capacity",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_sub_send,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(4096)),
                           &opt_ge64,
                           "events",
                           "Per-subscriber send queue cap"),
        SWITCH_CONFIG_ITEM("max_subscribers",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_max_sub,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(16)),
                           &opt_ge1,
                           "streams",
                           "Max concurrent SubscribeEvents streams"),

        // Idempotency
        SWITCH_CONFIG_ITEM("idempotency_ttl_seconds",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_idemp_ttl,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(300)),
                           &opt_ge1,
                           "seconds",
                           "Idempotency cache TTL"),
        SWITCH_CONFIG_ITEM("idempotency_cache_capacity",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_idemp_cap,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(1500)),
                           &opt_ge16,
                           "entries",
                           "Idempotency cache capacity"),
        SWITCH_CONFIG_ITEM("idempotency_in_flight_max_wait_seconds",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_idemp_max_wait,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(30)),
                           &opt_ge1,
                           "seconds",
                           "Idempotency in-flight shadow timeout"),

        // Drain
        SWITCH_CONFIG_ITEM("drain_timeout_seconds",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_drain_to,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(30)),
                           &opt_ge1,
                           "seconds",
                           "Drain max wait"),
        SWITCH_CONFIG_ITEM("event_drain_timeout_seconds",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_evdrain_to,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(5)),
                           &opt_ge1,
                           "seconds",
                           "Event-ring drain max wait"),

        // Terminate handler (default OFF per architecture.md §"Terminate handler chaining")
        SWITCH_CONFIG_ITEM("osw_panic_on_unhandled",
                           SWITCH_CONFIG_BOOL,
                           CONFIG_RELOADABLE,
                           &bool_panic,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(SWITCH_FALSE)),
                           nullptr,
                           "true|false",
                           "Install std::set_terminate handler"),
        SWITCH_CONFIG_ITEM("osw_install_signal_handlers",
                           SWITCH_CONFIG_BOOL,
                           CONFIG_RELOADABLE,
                           &bool_sigh,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(SWITCH_FALSE)),
                           nullptr,
                           "true|false",
                           "Install SIGSEGV/SIGABRT/SIGBUS handlers"),

        // Observability
        SWITCH_CONFIG_ITEM_CALLBACK("log_level",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "info",
                                    &StringCallback,
                                    &sp_log,
                                    "trace|debug|info|warn|error|crit",
                                    "Log level"),
        SWITCH_CONFIG_ITEM_CALLBACK("pii_redaction_patterns",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "",
                                    &StringCallback,
                                    &sp_pii,
                                    "pipe-separated regexes",
                                    "PII redaction patterns"),

        // Tenant ACL (W3 will consume tenant_allowed_contexts)
        SWITCH_CONFIG_ITEM_CALLBACK("tenant_allowed_contexts",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "",
                                    &StringCallback,
                                    &sp_acl,
                                    "tenant:ctx1,ctx2;...",
                                    "Per-tenant allowed dialplan contexts"),

        // W6 Track C — TTS playout buffer
        SWITCH_CONFIG_ITEM("tts_jitter_buffer_ms",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_tts_jitter,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(1000)),
                           &opt_ge200,
                           "ms",
                           "TTS playout buffer target depth (ms)"),
        SWITCH_CONFIG_ITEM("tts_preroll_ms",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_tts_preroll,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(500)),
                           &opt_ge50,
                           "ms",
                           "TTS playout pre-roll before first non-silence frame (ms)"),
        SWITCH_CONFIG_ITEM("tts_high_water_ms",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_tts_high_water,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(1500)),
                           &opt_ge200,
                           "ms",
                           "TTS playout high-water mark for drop-oldest (ms)"),
        SWITCH_CONFIG_ITEM("tts_max_jitter_buffer_ms",
                           SWITCH_CONFIG_INT,
                           CONFIG_RELOADABLE,
                           &int_tts_max_jitter,
                           reinterpret_cast<const void*>(static_cast<std::intptr_t>(5000)),
                           &opt_ge200,
                           "ms",
                           "Hard cap on per-call jitter buffer override (ms)"),
        SWITCH_CONFIG_ITEM_CALLBACK("tts_underrun_policy",
                                    SWITCH_CONFIG_CUSTOM,
                                    CONFIG_RELOADABLE,
                                    nullptr,
                                    "silence",
                                    &StringCallback,
                                    &sp_tts_underrun,
                                    "silence|repeat_last",
                                    "TTS underrun policy (silence or repeat_last)"),

        SWITCH_CONFIG_ITEM_END()};

    // FF-013: switch_xml_config_parse_module_settings opens the file,
    // walks the table, frees the XML, and returns
    // SWITCH_STATUS_SUCCESS / FALSE. FALSE here means the file
    // couldn't be opened — we surface that to the caller as `false`
    // but DO NOT overwrite the defaults already in `out`.
    const switch_status_t st =
        switch_xml_config_parse_module_settings(xml_file_name, SWITCH_FALSE, instructions);

    if (st == SWITCH_STATUS_SUCCESS) {
        // Copy parsed ints back into the typed fields.
        out.grpc_max_concurrent_streams = static_cast<std::uint32_t>(int_max_streams);
        out.grpc_drain_deadline_seconds = static_cast<std::uint32_t>(int_drain_deadline);
        out.event_ring_capacity_tier1 = static_cast<std::uint32_t>(int_ring_t1);
        out.event_ring_capacity_tier2 = static_cast<std::uint32_t>(int_ring_t2);
        out.event_ring_capacity_tier3 = static_cast<std::uint32_t>(int_ring_t3);
        out.subscriber_send_queue_capacity = static_cast<std::uint32_t>(int_sub_send);
        out.max_subscribers = static_cast<std::uint32_t>(int_max_sub);
        out.idempotency_ttl_seconds = static_cast<std::uint32_t>(int_idemp_ttl);
        out.idempotency_cache_capacity = static_cast<std::uint32_t>(int_idemp_cap);
        out.idempotency_in_flight_max_wait_seconds = static_cast<std::uint32_t>(int_idemp_max_wait);
        out.drain_timeout_seconds = static_cast<std::uint32_t>(int_drain_to);
        out.event_drain_timeout_seconds = static_cast<std::uint32_t>(int_evdrain_to);
        out.osw_panic_on_unhandled = (bool_panic == SWITCH_TRUE);
        out.osw_install_signal_handlers = (bool_sigh == SWITCH_TRUE);
        out.grpc_tls_require_client_cert = (bool_require_client_cert == SWITCH_TRUE);
        SplitPipeList(pii_pipe, out.pii_redaction_patterns);
        // W6 Track C — TTS playout buffer
        out.tts_jitter_buffer_ms = static_cast<std::uint32_t>(int_tts_jitter);
        out.tts_preroll_ms = static_cast<std::uint32_t>(int_tts_preroll);
        out.tts_high_water_ms = static_cast<std::uint32_t>(int_tts_high_water);
        out.tts_max_jitter_buffer_ms = static_cast<std::uint32_t>(int_tts_max_jitter);
        return true;
    }

    // File missing or unparseable. The instruction table's default
    // values were not applied (FS only applies defaults when the
    // <param> is present in the XML); but our pre-parse `out = Config{}`
    // already populated the C++ defaults, so the data is still
    // coherent. Return false so the caller logs the "config file
    // absent, using defaults" warning.
    return false;
}

}  // namespace osw
