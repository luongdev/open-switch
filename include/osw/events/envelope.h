/*
 * include/osw/events/envelope.h
 *
 * osw::events::BuildEnvelope — converts a FreeSWITCH event into the
 * wire-form open_switch::events::v1::EventEnvelope proto, populating
 * the W2-relevant fields.
 *
 * Called from inside the osw_event_handler callback (FF-018) — the
 * switch_event_t* is FS-owned and lives only for the callback's
 * duration. BuildEnvelope must read everything it needs synchronously
 * and never retain the FS pointer past return.
 *
 * Field population (per W2 contract §"src/events/envelope"):
 *   event_id        — UUIDv7 (time-ordered; generated here)
 *   tier            — caller-supplied (the classifier ran first)
 *   event_name      — switch_event_get_header(ev, "Event-Name") or
 *                     a numeric fallback if missing
 *   subclass_name   — switch_event_get_header(ev, "Event-Subclass") if any
 *   node_id         — caller-supplied (configured at module load)
 *   emitted_at      — Event-Date-Timestamp header → microseconds
 *                     converted to google.protobuf.Timestamp
 *   seq             — caller-supplied (per-tier atomic fetch_add)
 *   tenant_id       — channel variable "osw_tenant_id" if present
 *   traceparent     — channel variable "osw_traceparent" if present
 *   channel_uuid    — switch_event_get_header(ev, "Unique-ID")
 *   variables       — config-driven include-list of channel variables
 *   headers         — config-driven include / exclude lists
 *   body            — switch_event_get_body(ev) when non-empty
 *   schema_version  — 1
 *
 * Memory:
 *   Caller passes a google::protobuf::Arena*; the returned envelope is
 *   arena-owned. Single allocation per event. The caller resets/destroys
 *   the arena after serializing to bytes — the serialised bytes
 *   themselves are a separate shared_ptr<const string>.
 *
 * Header / variable include-list config:
 *   Captured in `EnvelopeBuildConfig`. Defaults provided by
 *   MakeDefaultEnvelopeConfig() match the W2 contract:
 *     headers_include: {"Caller-Caller-ID-Name", "Caller-Caller-ID-Number",
 *                       "Caller-Destination-Number", "Answer-State",
 *                       "Hangup-Cause", "Event-Date-Timestamp",
 *                       "Event-Name", "Event-Subclass", "Unique-ID"}
 *     variables_include: empty (operators add specific channel vars)
 *
 *   When `headers_include` is empty, ALL headers are forwarded. When
 *   non-empty, ONLY listed headers reach the envelope (always-include
 *   plus operator additions).
 *
 * FACTs cited:
 *   - FF-018: handler ownership — pointer is FS-owned, callback-scope.
 *   - FF-019: switch_event_get_header returns FS-owned char* — must copy
 *     synchronously.
 *   - FF-020: subclass arrives via "Event-Subclass" header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_ENVELOPE_H_
#define OSW_EVENTS_ENVELOPE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "osw/events/tier.h"

// Forward-declare FS opaque + arena to keep this header thin. The
// implementation file pulls in <switch.h> (via fs_api.h) and the proto.
extern "C" struct switch_event;
using switch_event_t = switch_event;
namespace google::protobuf {
class Arena;
}
namespace open_switch::events::v1 {
class EventEnvelope;
}

namespace osw::events {

/// Header / variable include-and-exclude lists, captured at module
/// load from osw::Config (W2 config extension lands alongside module
/// wiring).
struct EnvelopeBuildConfig {
    /// Forward only these header names. Empty list = forward ALL.
    std::unordered_set<std::string> headers_include;

    /// Always forward these regardless of headers_include.
    /// (Used for the well-known fields the proto schema dedicates a
    /// top-level field to — Event-Name, Event-Subclass, Unique-ID,
    /// Event-Date-Timestamp — we don't want to repeat them in
    /// EventEnvelope.headers, so they go in the exclude_after_extract
    /// set instead.)
    std::unordered_set<std::string> headers_always_exclude;

    /// Forward channel variables matching one of these names. Empty =
    /// don't forward variables.
    std::unordered_set<std::string> variables_include;
};

/// Defaults documented in the header comment.
[[nodiscard]] EnvelopeBuildConfig MakeDefaultEnvelopeConfig();

/// Build an envelope from `ev`. Caller owns `arena` and the returned
/// pointer is arena-allocated. The function reads headers + body
/// synchronously from FS-owned memory (FF-019) and never retains the
/// FS pointer.
///
/// `tier`, `seq`, and `node_id` are caller-supplied:
///   - tier: from the classifier on the same (event_name, subclass)
///   - seq: from a per-tier std::atomic<uint64_t> fetch_add at the
///     producer (FF-004 makes the atomic correct; the broadcaster sees
///     seqs in producer-acquisition order, NOT seq order — but the
///     subscriber's since_seq replay query sorts via the ring's
///     SnapshotFromSeq).
///   - node_id: the configured `node_id` from osw::Config (a host name
///     or operator-set id; documented per-deploy).
///
/// On any failure (null ev, allocation failure, unrecoverable proto
/// error), returns nullptr. Never throws.
[[nodiscard]] open_switch::events::v1::EventEnvelope* BuildEnvelope(
    switch_event_t*                       ev,
    Tier                                  tier,
    std::uint64_t                         seq,
    std::string_view                      node_id,
    const EnvelopeBuildConfig&            cfg,
    google::protobuf::Arena*              arena) noexcept;

// --- Helpers, exposed for testing -----------------------------------

/// Generate a UUIDv7 string (RFC-9562 layout, lowercase hex with hyphens).
/// Time-ordered: the first 48 bits are unix milliseconds, then 4-bit
/// version 7, then 12 random bits, then 2-bit variant 10, then 62
/// random bits.
///
/// Implementation note: this does NOT use libuuid — it's a thin self-
/// contained generator. libuuid in older util-linux versions doesn't
/// support UUIDv7, and we want every dispatch-thread invocation to be
/// allocation-free in the hot path. The implementation uses a
/// thread_local std::mt19937_64 seeded from random_device + the thread
/// id; collisions are astronomically unlikely.
[[nodiscard]] std::string GenerateUuidV7() noexcept;

/// Parse an FS "Event-Date-Timestamp" header (microseconds since
/// epoch, decimal). Returns 0 on parse failure (callers treat as
/// "unknown timestamp" → emit current time as the fallback).
[[nodiscard]] std::int64_t ParseEventTimestampMicros(std::string_view s) noexcept;

}  // namespace osw::events

#endif  // OSW_EVENTS_ENVELOPE_H_
