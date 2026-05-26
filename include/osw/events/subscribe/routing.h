/*
 * include/osw/events/subscribe/routing.h
 *
 * Lightweight scanner that extracts the routing-relevant fields
 * (tier, event_name, node_id) from a serialised
 * `open_switch.events.v1.EventEnvelope`. Used by the broadcaster's
 * live-tail dispatch path AND by the SubscribeEvents handler's
 * since_seq replay path so the subscriber filter is applied
 * symmetrically across both.
 *
 * The scanner does a manual proto wire-format walk over the bytes;
 * we avoid a full `ParseFromString` per entry per subscriber on the
 * hot path. The bytes themselves stay as
 * `shared_ptr<const std::string>` (zero copy) — only the routing
 * fields are decoded.
 *
 * Wire-format coverage (proto3):
 *   - field 2 (tier, enum)            — varint
 *   - field 3 (event_name, string)    — length-delimited
 *   - field 4 (subclass_name, string) — length-delimited
 *   - field 5 (node_id, string)       — length-delimited
 *
 * Any other field is skipped via the standard wire-type rules
 * (varint / 64-bit / length-delimited / 32-bit). Malformed input
 * causes a conservative all-zero result; the subscriber filter
 * treats it as "unknown tier / empty name", which matches no
 * narrowed filter (so the entry is dropped — fail-closed against
 * malformed bytes).
 *
 * Thread-safety: the scanner reads the input bytes only; the caller
 * owns the lifetime of `bytes` (typically a `shared_ptr<const string>`
 * sitting in a ring or a subscriber's send queue).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_SUBSCRIBE_ROUTING_H_
#define OSW_EVENTS_SUBSCRIBE_ROUTING_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "osw/events/tier.h"

namespace osw::events {

/// The subset of EventEnvelope fields needed to run a subscriber
/// filter without a full proto parse.
struct RoutingFields {
    Tier tier = Tier::kUnspecified;
    std::string_view event_name;
    std::string_view subclass_name;  // empty for non-CUSTOM events
    std::string_view node_id;
};

/// Decode the routing fields from a serialised EventEnvelope. Never
/// throws; on malformed input returns a default-constructed
/// `RoutingFields` (which causes any narrowed subscriber filter to
/// reject the entry — fail-closed).
[[nodiscard]] RoutingFields ExtractRoutingFields(const std::string& bytes) noexcept;

}  // namespace osw::events

#endif  // OSW_EVENTS_SUBSCRIBE_ROUTING_H_
