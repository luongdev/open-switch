/*
 * src/events/envelope.cc
 *
 * Implementation of osw::events::BuildEnvelope. See header for the
 * contract.
 *
 * Build-mode contract (matches the W1 RAII helpers + the W2 audit
 * helper):
 *
 *   - Production build: compiled into osw_events_fs WITHOUT
 *     OSW_TEST_FS_MOCK. fs_api.h pulls in <switch.h> and the FS
 *     header accessors route to the real switch_event_get_*
 *     functions.
 *
 *   - Test build: compiled into osw_events_test_helpers with
 *     -DOSW_TEST_FS_MOCK=1. fs_api.h includes fs_mock.h and the
 *     header accessors return strings the test pre-populated in
 *     MockState::events_by_ptr.
 *
 * The TU itself is unaware of which mode it's in.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/envelope.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <google/protobuf/arena.h>

#include "open_switch/events/v1/events.pb.h"

#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

namespace osw::events {

namespace {

constexpr const char* kSubsystem = "events.envelope";

// Header keys we consult directly. These are populated by FreeSWITCH
// at event-create time for channel-related events
// (switch_channel_event_set_data writes Unique-ID, Caller-*, etc.).
constexpr const char* kHdrEventName = "Event-Name";
constexpr const char* kHdrEventSubclass = "Event-Subclass";
constexpr const char* kHdrEventTimestamp = "Event-Date-Timestamp";
constexpr const char* kHdrUniqueId = "Unique-ID";

// Channel-variable header convention: FS prefixes channel variables on
// the event with "variable_<name>" when switch_channel_event_set_data
// runs (see FS src/switch_channel.c — switch_channel_get_variables /
// callbacks). The audit subclass family doesn't have these because
// audit events are CUSTOM, not channel-bound.
constexpr const char* kVariablePrefix = "variable_";

// Convention'd osw_* channel variables we promote to top-level fields.
constexpr const char* kVarTenantId = "variable_osw_tenant_id";
constexpr const char* kVarTraceparent = "variable_osw_traceparent";

open_switch::events::v1::Tier MapTierToProto(Tier t) noexcept {
    switch (t) {
        case Tier::k1Critical:
            return open_switch::events::v1::TIER_1_CRITICAL;
        case Tier::k2State:
            return open_switch::events::v1::TIER_2_STATE;
        case Tier::k3Ephemeral:
            return open_switch::events::v1::TIER_3_EPHEMERAL;
        case Tier::kUnspecified:
        default:
            return open_switch::events::v1::TIER_UNSPECIFIED;
    }
}

// FF-019: switch_event_get_header may return NULL — defensively wrap to
// std::string_view("") so we don't pass NULL to std::string ctor.
std::string_view HeaderOr(switch_event_t* ev,
                          const char* name,
                          std::string_view fallback = "") noexcept {
    const char* v = ::osw::raii::fs::EventGetHeader(ev, name);
    return v ? std::string_view(v) : fallback;
}

std::string HeaderOrEmpty(switch_event_t* ev, const char* name) noexcept {
    return std::string(HeaderOr(ev, name));
}

}  // namespace

EnvelopeBuildConfig MakeDefaultEnvelopeConfig() {
    EnvelopeBuildConfig cfg;

    // Forward only the safe-by-default header set. Operators can extend
    // via XML config (parsed by the module-wiring commit).
    cfg.headers_include = {
        "Caller-Caller-ID-Name",
        "Caller-Caller-ID-Number",
        "Caller-Destination-Number",
        "Caller-Direction",
        "Caller-Context",
        "Answer-State",
        "Hangup-Cause",
        "Hangup-Cause-Q.850",
        "Channel-State",
        "Channel-Call-State",
        "Channel-Name",
    };

    // These are promoted to top-level proto fields; do NOT also duplicate
    // them into EventEnvelope.headers.
    cfg.headers_always_exclude = {
        "Event-Name",
        "Event-Subclass",
        "Event-Date-Timestamp",
        "Unique-ID",
    };

    // No channel variables by default. Operators opt in by listing names
    // (the include set matches against the variable name AFTER stripping
    // the "variable_" prefix FS attaches).
    cfg.variables_include = {};
    return cfg;
}

std::int64_t ParseEventTimestampMicros(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::int64_t out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return 0;  // FS always emits decimal; anything else is malformed
        }
        // Guard against overflow (microseconds since 1970 fits in i64).
        if (out > (INT64_MAX / 10)) {
            return 0;
        }
        out = out * 10 + (c - '0');
    }
    return out;
}

std::string GenerateUuidV7() noexcept {
    // Thread-local 64-bit Mersenne Twister. Seeded once per thread from
    // random_device | thread_id | a high-res steady_clock sample so that
    // collisions across threads are astronomically unlikely.
    struct Rng {
        std::mt19937_64 gen;
        Rng() {
            std::random_device rd;
            std::seed_seq seq{
                rd(),
                rd(),
                rd(),
                rd(),
                static_cast<std::uint32_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count()),
            };
            gen.seed(seq);
        }
    };
    thread_local Rng rng;

    // Unix milliseconds since epoch — 48 bits.
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::uint64_t ts_ms = static_cast<std::uint64_t>(ms) & 0x0000FFFFFFFFFFFFULL;

    // 64 bits of randomness split into rand_a (12) + variant_top (2) +
    // rand_b (62 - 0 = we use 64; mask variant in place).
    const std::uint64_t r1 = rng.gen();  // for rand_a
    const std::uint64_t r2 = rng.gen();  // for rand_b

    // Compose:
    //   bytes 0..5 : ts_ms (big-endian)
    //   byte  6    : 0x70 | (rand_a_high4 & 0x0F)
    //   byte  7    : rand_a_low8
    //   byte  8    : 0x80 | (rand_b_high6 & 0x3F)
    //   bytes 9..15: rand_b_remaining56
    std::uint8_t b[16];
    b[0] = static_cast<std::uint8_t>((ts_ms >> 40) & 0xFF);
    b[1] = static_cast<std::uint8_t>((ts_ms >> 32) & 0xFF);
    b[2] = static_cast<std::uint8_t>((ts_ms >> 24) & 0xFF);
    b[3] = static_cast<std::uint8_t>((ts_ms >> 16) & 0xFF);
    b[4] = static_cast<std::uint8_t>((ts_ms >> 8) & 0xFF);
    b[5] = static_cast<std::uint8_t>((ts_ms >> 0) & 0xFF);

    const std::uint16_t rand_a = static_cast<std::uint16_t>(r1 & 0x0FFF);
    b[6] = static_cast<std::uint8_t>(0x70 | ((rand_a >> 8) & 0x0F));
    b[7] = static_cast<std::uint8_t>(rand_a & 0xFF);

    // Variant 10xxxxxx in the top 2 bits of byte 8.
    b[8] = static_cast<std::uint8_t>(0x80 | ((r2 >> 56) & 0x3F));
    b[9] = static_cast<std::uint8_t>((r2 >> 48) & 0xFF);
    b[10] = static_cast<std::uint8_t>((r2 >> 40) & 0xFF);
    b[11] = static_cast<std::uint8_t>((r2 >> 32) & 0xFF);
    b[12] = static_cast<std::uint8_t>((r2 >> 24) & 0xFF);
    b[13] = static_cast<std::uint8_t>((r2 >> 16) & 0xFF);
    b[14] = static_cast<std::uint8_t>((r2 >> 8) & 0xFF);
    b[15] = static_cast<std::uint8_t>((r2 >> 0) & 0xFF);

    // Format as 8-4-4-4-12 lowercase hex. 36 chars + null.
    char out[37];
    std::snprintf(out,
                  sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0],
                  b[1],
                  b[2],
                  b[3],
                  b[4],
                  b[5],
                  b[6],
                  b[7],
                  b[8],
                  b[9],
                  b[10],
                  b[11],
                  b[12],
                  b[13],
                  b[14],
                  b[15]);
    return std::string(out, 36);
}

open_switch::events::v1::EventEnvelope* BuildEnvelope(switch_event_t* ev,
                                                      Tier tier,
                                                      std::uint64_t seq,
                                                      std::string_view node_id,
                                                      const EnvelopeBuildConfig& cfg,
                                                      google::protobuf::Arena* arena) noexcept {
    if (ev == nullptr || arena == nullptr) {
        return nullptr;
    }

    try {
        auto* env = google::protobuf::Arena::Create<open_switch::events::v1::EventEnvelope>(arena);
        if (env == nullptr) {
            return nullptr;
        }

        // event_id — UUIDv7, generated synchronously.
        env->set_event_id(GenerateUuidV7());

        // tier + seq + node_id + schema_version are caller-supplied.
        env->set_tier(MapTierToProto(tier));
        env->set_seq(seq);
        env->set_node_id(std::string(node_id));
        env->set_schema_version(1);

        // Well-known headers → top-level proto fields. FF-019 says the
        // returned const char* lives only while ev does; std::string
        // here copies into proto-arena storage immediately.
        env->set_event_name(HeaderOrEmpty(ev, kHdrEventName));
        env->set_subclass_name(HeaderOrEmpty(ev, kHdrEventSubclass));
        env->set_channel_uuid(HeaderOrEmpty(ev, kHdrUniqueId));

        // emitted_at: parse Event-Date-Timestamp (microseconds since epoch).
        // Fallback to "now" if missing/malformed.
        const std::int64_t micros = [&]() noexcept -> std::int64_t {
            const std::int64_t parsed = ParseEventTimestampMicros(HeaderOr(ev, kHdrEventTimestamp));
            if (parsed > 0)
                return parsed;
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }();
        auto* ts = env->mutable_emitted_at();
        ts->set_seconds(micros / 1'000'000);
        ts->set_nanos(static_cast<std::int32_t>((micros % 1'000'000) * 1'000));

        // tenant_id + traceparent — channel variables FS attached as
        // "variable_osw_*" headers (audit events are CUSTOM and have no
        // channel context, so these will be empty for those; that's fine).
        env->set_tenant_id(HeaderOrEmpty(ev, kVarTenantId));
        env->set_traceparent(HeaderOrEmpty(ev, kVarTraceparent));

        // Variables: walk headers with prefix "variable_" and emit if
        // the suffix is in cfg.variables_include. (Pre-W2 the variables
        // map stays empty.)
        //
        // FS exposes header walking via the switch_event_t::headers
        // chain. Because we route through fs_api.h's shim for testing,
        // we expose a single-header lookup only; the include-list scan
        // is per-name lookup, which is fine when the include list is
        // small (operator-curated).
        for (const auto& var : cfg.variables_include) {
            const std::string header_name = std::string(kVariablePrefix) + var;
            const char* val = ::osw::raii::fs::EventGetHeader(ev, header_name.c_str());
            if (val != nullptr) {
                (*env->mutable_variables())[var] = val;
            }
        }

        // Headers: same name-by-name lookup. The include list is small
        // by default; operators extend it explicitly. Excludes
        // (Event-Name, Event-Subclass, Event-Date-Timestamp, Unique-ID)
        // are skipped to avoid duplicating with the top-level fields.
        for (const auto& h : cfg.headers_include) {
            if (cfg.headers_always_exclude.count(h) > 0) {
                continue;
            }
            const char* val = ::osw::raii::fs::EventGetHeader(ev, h.c_str());
            if (val != nullptr) {
                (*env->mutable_headers())[h] = val;
            }
        }

        // Body: switch_event_get_body returns FS-owned char* (FF-019
        // ownership applies). Copy into the proto's bytes field.
        //
        // Gemini W2.5 N-4: FreeSWITCH event bodies are text-only — the
        // `body` slot is a `char*` allocated via `strdup` (DUP) inside
        // switch_event.c and FS's own serialisers (switch_event_serialize,
        // _json) measure length with strlen. There is no body-length
        // field exposed on switch_event_t (v1.10.12 verified). Embedded
        // NUL bytes are therefore truncated by FS itself before our
        // handler ever sees the event — the strlen() call below
        // matches FS's own semantics rather than introducing one.
        //
        // We pass the explicit length to proto's set_body(ptr, len)
        // overload to make the intent obvious to readers and to keep
        // the call out of the `set_body(const char*) → strlen` overload
        // that Gemini originally flagged. Subscribers MUST treat
        // EventEnvelope.body as text. See event-tiers.md §"Event body".
        const char* body = ::osw::raii::fs::EventGetBody(ev);
        if (body != nullptr && body[0] != '\0') {
            const std::size_t body_len = std::strlen(body);
            env->set_body(body, body_len);
        }

        return env;
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "BuildEnvelope threw: %s", e.what());
        return nullptr;
    } catch (...) {
        osw::log::Error(kSubsystem, "BuildEnvelope threw unknown exception");
        return nullptr;
    }
}

}  // namespace osw::events
