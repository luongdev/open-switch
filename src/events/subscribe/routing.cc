/*
 * src/events/subscribe/routing.cc
 *
 * Implementation of `osw::events::ExtractRoutingFields` — a manual
 * proto-wire-format scanner for the three fields the subscriber
 * filter needs. Originally lived inside the broadcaster TU; lifted
 * to a shared library so the SubscribeEvents handler can run the
 * same filter symmetrically on replay entries (Codex W2 finding C-1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/subscribe/routing.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace osw::events {

namespace {

[[nodiscard]] bool ReadVarint(const std::uint8_t*& p,
                              const std::uint8_t* end,
                              std::uint64_t& out) noexcept {
    out = 0;
    int shift = 0;
    while (p < end) {
        const std::uint8_t b = *p++;
        out |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0)
            return true;
        shift += 7;
        if (shift >= 64)
            return false;  // malformed
    }
    return false;
}

[[nodiscard]] bool SkipField(const std::uint8_t*& p,
                             const std::uint8_t* end,
                             int wire_type) noexcept {
    switch (wire_type) {
        case 0: {  // varint
            std::uint64_t tmp = 0;
            return ReadVarint(p, end, tmp);
        }
        case 1: {  // 64-bit fixed
            if (end - p < 8)
                return false;
            p += 8;
            return true;
        }
        case 2: {  // length-delimited
            std::uint64_t len = 0;
            if (!ReadVarint(p, end, len))
                return false;
            if (static_cast<std::uint64_t>(end - p) < len)
                return false;
            p += len;
            return true;
        }
        case 5: {  // 32-bit fixed
            if (end - p < 4)
                return false;
            p += 4;
            return true;
        }
        default:
            return false;  // unsupported (groups, etc.)
    }
}

}  // namespace

RoutingFields ExtractRoutingFields(const std::string& bytes) noexcept {
    RoutingFields rf;
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto* end = p + bytes.size();
    while (p < end) {
        std::uint64_t key = 0;
        if (!ReadVarint(p, end, key))
            break;
        const int field_num = static_cast<int>(key >> 3);
        const int wire_type = static_cast<int>(key & 0x7);

        if (field_num == 2 && wire_type == 0) {
            // tier (enum) — proto3 varint.
            std::uint64_t v = 0;
            if (!ReadVarint(p, end, v))
                break;
            switch (v) {
                case 1:
                    rf.tier = Tier::k1Critical;
                    break;
                case 2:
                    rf.tier = Tier::k2State;
                    break;
                case 3:
                    rf.tier = Tier::k3Ephemeral;
                    break;
                default:
                    rf.tier = Tier::kUnspecified;
                    break;
            }
        } else if (field_num == 3 && wire_type == 2) {
            // event_name (string)
            std::uint64_t len = 0;
            if (!ReadVarint(p, end, len))
                break;
            if (static_cast<std::uint64_t>(end - p) < len)
                break;
            rf.event_name =
                std::string_view(reinterpret_cast<const char*>(p), static_cast<std::size_t>(len));
            p += len;
        } else if (field_num == 5 && wire_type == 2) {
            // node_id (string)
            std::uint64_t len = 0;
            if (!ReadVarint(p, end, len))
                break;
            if (static_cast<std::uint64_t>(end - p) < len)
                break;
            rf.node_id =
                std::string_view(reinterpret_cast<const char*>(p), static_cast<std::size_t>(len));
            p += len;
        } else {
            if (!SkipField(p, end, wire_type))
                break;
        }
    }
    return rf;
}

}  // namespace events
}  // namespace osw
