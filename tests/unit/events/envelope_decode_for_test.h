/*
 * tests/unit/events/envelope_decode_for_test.h
 *
 * Test-only proto wire-format scanner that extracts the fields needed
 * by binder + subscribe_replay tests WITHOUT touching protobuf's
 * generated code. The point is to avoid `EventEnvelope::ParseFromString`
 * under TSAN: protobuf compiled without -fsanitize=thread (it lives at
 * /opt/grpc/lib/libprotobuf.a in the base image) has hash-table memset
 * patterns that TSAN's shadow memory cannot follow, producing a
 * deadly-SEGV inside __tsan_memset when `MapFieldBase::Clear` runs at
 * the start of `ParseFromString`. The SEGV is a runtime artefact of
 * mixing TSAN-instrumented and uninstrumented code, not a real race.
 *
 * This scanner reads only the fields the tests assert on (tier,
 * event_name, node_id, seq, schema_version) so the protobuf map fields
 * (variables=11, headers=12) are never touched.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_TESTS_UNIT_EVENTS_ENVELOPE_DECODE_FOR_TEST_H_
#define OSW_TESTS_UNIT_EVENTS_ENVELOPE_DECODE_FOR_TEST_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace osw::events::test {

struct DecodedEnvelope {
    int tier = 0;  // matches open_switch.events.v1.Tier enum values
    std::string_view event_name;
    std::string_view node_id;
    std::uint64_t seq = 0;
    std::uint32_t schema_version = 0;
    bool ok = false;
};

namespace detail {

inline bool ReadVarint(const std::uint8_t*& p,
                       const std::uint8_t* end,
                       std::uint64_t& out) noexcept {
    std::uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        const std::uint8_t b = *p++;
        v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            out = v;
            return true;
        }
        shift += 7;
        if (shift >= 64)
            return false;
    }
    return false;
}

inline bool SkipField(const std::uint8_t*& p,
                      const std::uint8_t* end,
                      int wire_type) noexcept {
    if (wire_type == 0) {  // varint
        std::uint64_t dummy = 0;
        return ReadVarint(p, end, dummy);
    }
    if (wire_type == 1) {  // 64-bit fixed
        if (end - p < 8)
            return false;
        p += 8;
        return true;
    }
    if (wire_type == 2) {  // length-delimited
        std::uint64_t len = 0;
        if (!ReadVarint(p, end, len))
            return false;
        if (static_cast<std::uint64_t>(end - p) < len)
            return false;
        p += len;
        return true;
    }
    if (wire_type == 5) {  // 32-bit fixed
        if (end - p < 4)
            return false;
        p += 4;
        return true;
    }
    return false;  // unknown / deprecated group wire types
}

}  // namespace detail

/// Decode the small subset of EventEnvelope fields the tests assert
/// on, using a manual wire-format walk. Never calls protobuf APIs.
/// Returns `ok=false` on malformed input.
inline DecodedEnvelope DecodeEnvelopeForTest(const std::string& bytes) noexcept {
    DecodedEnvelope d;
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto* end = p + bytes.size();
    while (p < end) {
        std::uint64_t key = 0;
        if (!detail::ReadVarint(p, end, key))
            return d;
        const int field_num = static_cast<int>(key >> 3);
        const int wire_type = static_cast<int>(key & 0x7);

        if (field_num == 2 && wire_type == 0) {
            std::uint64_t v = 0;
            if (!detail::ReadVarint(p, end, v))
                return d;
            d.tier = static_cast<int>(v);
        } else if (field_num == 3 && wire_type == 2) {
            std::uint64_t len = 0;
            if (!detail::ReadVarint(p, end, len))
                return d;
            if (static_cast<std::uint64_t>(end - p) < len)
                return d;
            d.event_name = std::string_view(reinterpret_cast<const char*>(p), len);
            p += len;
        } else if (field_num == 5 && wire_type == 2) {
            std::uint64_t len = 0;
            if (!detail::ReadVarint(p, end, len))
                return d;
            if (static_cast<std::uint64_t>(end - p) < len)
                return d;
            d.node_id = std::string_view(reinterpret_cast<const char*>(p), len);
            p += len;
        } else if (field_num == 7 && wire_type == 0) {
            std::uint64_t v = 0;
            if (!detail::ReadVarint(p, end, v))
                return d;
            d.seq = v;
        } else if (field_num == 14 && wire_type == 0) {
            std::uint64_t v = 0;
            if (!detail::ReadVarint(p, end, v))
                return d;
            d.schema_version = static_cast<std::uint32_t>(v);
        } else {
            if (!detail::SkipField(p, end, wire_type))
                return d;
        }
    }
    d.ok = true;
    return d;
}

}  // namespace osw::events::test

#endif  // OSW_TESTS_UNIT_EVENTS_ENVELOPE_DECODE_FOR_TEST_H_
