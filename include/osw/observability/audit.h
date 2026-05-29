/*
 * include/osw/observability/audit.h
 *
 * osw::audit — helper for firing CUSTOM/osw.audit.* events into the
 * FreeSWITCH event bus. The audit family is the module's Tier-1
 * structured-log channel: every audit emission goes through the FS
 * event facility and re-enters our own pipeline (W2 Binder →
 * classifier → Tier-1 ring → subscribers). Subscribers see them
 * exactly like any other Tier-1 event, with `subclass_name = "osw.audit.<name>"`.
 *
 * Subclass family (W2 ships):
 *   - osw.audit.module_loaded
 *   - osw.audit.subscriber_connected
 *   - osw.audit.subscriber_disconnected
 *   - osw.audit.subscriber_kicked (queue_full / RESOURCE_EXHAUSTED)
 *
 * Subscribers filter by `subclass_name` prefix `osw.audit.` to
 * receive only the audit channel.
 *
 * Removed in W2.5 (Codex review B-2): the
 * `osw.audit.module_shutdown_with_pending_events` audit was
 * dead-lettered for gRPC subscribers because it was emitted AFTER
 * Binder::Stop() — the binder was unbound, so switch_event_fire
 * never re-entered our pipeline. Operators consume the equivalent
 * signal via the FS-log "module_shutdown_drain_timeout" WARN line
 * plus the Health.tierN_dropped_total counters.
 *
 * FACTs cited:
 *   - FF-017 (event lifecycle), FF-020 (create_subclass + Event-Subclass).
 *
 * Threading:
 *   - Emit() is callable from any thread. It calls
 *     switch_event_create_subclass (FF-020) which is thread-safe
 *     (internal CUSTOM_HASH mutex + per-thread alloc), then fires
 *     via switch_event_fire (FF-017) which is thread-safe (internal
 *     dispatch queue mutex).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_AUDIT_H_
#define OSW_OBSERVABILITY_AUDIT_H_

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osw::audit {

/// (header_name, header_value) pair shipped with an audit event.
struct Header {
    std::string name;
    std::string value;
};

/// Convenience builder so call sites can write
///   osw::audit::Emit("module_loaded", {{"module_version","0.1.0"}});
/// without spelling out a vector.
using HeadersInit = std::initializer_list<Header>;

/// Subclass prefix the helper prepends. All audit events have a
/// subclass_name of `kSubclassPrefix + name`. Public constant so
/// the tier classifier and subscribers can reference the same value.
inline constexpr std::string_view kSubclassPrefix = "osw.audit.";

/// Fire an audit CUSTOM event. Returns true on success, false on any
/// failure path (allocation failure, FS shutting down, etc.). Never
/// throws — exceptions are caught and logged.
///
/// `name` is the short suffix; the full subclass is
/// `osw.audit.<name>`. Caller-supplied `headers` are appended after
/// the FS-side `Event-Subclass` header.
///
/// The audit subclass family is reserved by convention only — we do
/// NOT call switch_event_reserve_subclass at module load (see
/// FF-020 "Implications"); the FS bind path auto-reserves on demand.
bool Emit(std::string_view name, const std::vector<Header>& headers) noexcept;

/// Fire a CUSTOM event using an exact subclass name. This is for documented
/// module event families that intentionally do not live under osw.audit.*,
/// such as W7 recording lifecycle/quality subclasses.
bool EmitSubclass(std::string_view subclass, const std::vector<Header>& headers) noexcept;

/// Convenience overload taking an initializer list of headers.
bool Emit(std::string_view name, HeadersInit headers) noexcept;
bool EmitSubclass(std::string_view subclass, HeadersInit headers) noexcept;

/// Convenience overload for no headers.
inline bool Emit(std::string_view name) noexcept {
    return Emit(name, std::vector<Header>{});
}

inline bool EmitSubclass(std::string_view subclass) noexcept {
    return EmitSubclass(subclass, std::vector<Header>{});
}

}  // namespace osw::audit

#endif  // OSW_OBSERVABILITY_AUDIT_H_
