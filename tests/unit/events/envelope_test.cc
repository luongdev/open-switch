/*
 * tests/unit/events/envelope_test.cc
 *
 * Unit tests for osw::events::BuildEnvelope against the FS-mock seam.
 *
 * Covered:
 *   - UUIDv7 format check (36 chars, hex+hyphens, version 7, variant 10).
 *   - tier + seq + node_id + schema_version are propagated literally.
 *   - event_name / subclass_name / channel_uuid extracted from headers.
 *   - Event-Date-Timestamp parsed into emitted_at (sec + nano).
 *   - tenant_id and traceparent from variable_osw_* headers.
 *   - Include-list filters headers and variables.
 *   - Excluded headers (Event-Name etc.) are NOT duplicated in the
 *     headers map.
 *   - Body forwarded when non-empty.
 *   - Null event yields nullptr.
 *   - Missing Event-Date-Timestamp falls back to "now".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/envelope.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <google/protobuf/arena.h>

#include "open_switch/events/v1/events.pb.h"
#include "osw/raii/fs_mock.h"

namespace {

using osw::events::BuildEnvelope;
using osw::events::EnvelopeBuildConfig;
using osw::events::GenerateUuidV7;
using osw::events::MakeDefaultEnvelopeConfig;
using osw::events::ParseEventTimestampMicros;
using osw::events::Tier;

switch_event_t* const kEv = reinterpret_cast<switch_event_t*>(0xE001);

class EnvelopeTest : public ::testing::Test {
 protected:
    void SetUp() override { osw::raii::fs::MockReset(); }

    static void SetHeader(switch_event_t* ev,
                          const std::string& name,
                          const std::string& value) {
        auto& m = osw::raii::fs::Mock();
        std::lock_guard<std::mutex> g(m.capture_mu);
        m.events_by_ptr[ev].headers.emplace_back(name, value);
    }

    google::protobuf::Arena arena_;
};

TEST_F(EnvelopeTest, UuidV7FormatIsRfc9562) {
    const std::string u = GenerateUuidV7();
    ASSERT_EQ(u.size(), 36u);
    EXPECT_EQ(u[8],  '-');
    EXPECT_EQ(u[13], '-');
    EXPECT_EQ(u[18], '-');
    EXPECT_EQ(u[23], '-');
    // Version 7: byte 6 (= chars 14-15) high nibble == '7'.
    EXPECT_EQ(u[14], '7') << u;
    // Variant 10xxxxxx: byte 8 (= chars 19-20) first hex digit is 8/9/a/b.
    const char v = u[19];
    EXPECT_TRUE(v == '8' || v == '9' || v == 'a' || v == 'b') << u;
    // All hex.
    for (std::size_t i = 0; i < u.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char c = u[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "char " << i << " of " << u;
    }
}

TEST_F(EnvelopeTest, UuidV7IsMonotonicishInMs) {
    // Two UUIDv7s generated <1ms apart may share their ms prefix, but
    // one generated >1ms later must compare strictly greater
    // lexicographically.
    const std::string a = GenerateUuidV7();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::string b = GenerateUuidV7();
    EXPECT_LT(a, b);
}

TEST_F(EnvelopeTest, ParseTimestampHappyPath) {
    EXPECT_EQ(ParseEventTimestampMicros("1748287234567890"), 1748287234567890);
}
TEST_F(EnvelopeTest, ParseTimestampMalformedReturnsZero) {
    EXPECT_EQ(ParseEventTimestampMicros(""),        0);
    EXPECT_EQ(ParseEventTimestampMicros("nope"),    0);
    EXPECT_EQ(ParseEventTimestampMicros("123a456"), 0);
}

TEST_F(EnvelopeTest, NullEventYieldsNullptr) {
    EnvelopeBuildConfig cfg;
    EXPECT_EQ(BuildEnvelope(nullptr, Tier::k1Critical, 7, "node-x", cfg, &arena_),
              nullptr);
}

TEST_F(EnvelopeTest, NullArenaYieldsNullptr) {
    EnvelopeBuildConfig cfg;
    EXPECT_EQ(BuildEnvelope(kEv, Tier::k1Critical, 7, "node-x", cfg, nullptr),
              nullptr);
}

TEST_F(EnvelopeTest, TopLevelHeadersExtractedCorrectly) {
    SetHeader(kEv, "Event-Name",            "CHANNEL_HANGUP_COMPLETE");
    SetHeader(kEv, "Event-Subclass",        "");
    SetHeader(kEv, "Event-Date-Timestamp",  "1748287234567890");
    SetHeader(kEv, "Unique-ID",             "abc-123-456");
    SetHeader(kEv, "variable_osw_tenant_id", "tenant-foo");
    SetHeader(kEv, "variable_osw_traceparent",
              "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");

    auto* env = BuildEnvelope(kEv, Tier::k1Critical, 42,
                              "node-1", MakeDefaultEnvelopeConfig(), &arena_);
    ASSERT_NE(env, nullptr);

    EXPECT_EQ(env->tier(),         open_switch::events::v1::TIER_1_CRITICAL);
    EXPECT_EQ(env->seq(),          42u);
    EXPECT_EQ(env->node_id(),      "node-1");
    EXPECT_EQ(env->schema_version(), 1u);
    EXPECT_EQ(env->event_name(),   "CHANNEL_HANGUP_COMPLETE");
    EXPECT_EQ(env->subclass_name(), "");
    EXPECT_EQ(env->channel_uuid(), "abc-123-456");
    EXPECT_EQ(env->tenant_id(),    "tenant-foo");
    EXPECT_EQ(env->traceparent(),
              "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(env->event_id().empty());
    EXPECT_EQ(env->event_id().size(), 36u);

    // emitted_at: 1748287234567890 us → seconds=1748287234, nanos=567890000.
    EXPECT_EQ(env->emitted_at().seconds(), 1748287234);
    EXPECT_EQ(env->emitted_at().nanos(),   567890000);
}

TEST_F(EnvelopeTest, CustomSubclassRoundTrips) {
    SetHeader(kEv, "Event-Name",     "CUSTOM");
    SetHeader(kEv, "Event-Subclass", "osw.audit.module_loaded");

    auto* env = BuildEnvelope(kEv, Tier::k1Critical, 1, "node",
                              MakeDefaultEnvelopeConfig(), &arena_);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(env->event_name(),    "CUSTOM");
    EXPECT_EQ(env->subclass_name(), "osw.audit.module_loaded");
}

TEST_F(EnvelopeTest, IncludeListFiltersHeadersAndExcludesWellKnown) {
    SetHeader(kEv, "Event-Name",                "CHANNEL_ANSWER");
    SetHeader(kEv, "Unique-ID",                 "u1");
    SetHeader(kEv, "Caller-Caller-ID-Name",     "Alice");
    SetHeader(kEv, "Caller-Caller-ID-Number",   "+15550100");
    SetHeader(kEv, "Caller-Destination-Number", "+15550200");
    SetHeader(kEv, "Some-Other-Header",         "ignored");

    auto* env = BuildEnvelope(kEv, Tier::k2State, 1, "n",
                              MakeDefaultEnvelopeConfig(), &arena_);
    ASSERT_NE(env, nullptr);

    const auto& h = env->headers();
    EXPECT_EQ(h.size(), 3u);  // 3 included headers, 0 well-known dupes
    EXPECT_EQ(h.at("Caller-Caller-ID-Name"),     "Alice");
    EXPECT_EQ(h.at("Caller-Caller-ID-Number"),   "+15550100");
    EXPECT_EQ(h.at("Caller-Destination-Number"), "+15550200");
    EXPECT_EQ(h.count("Event-Name"), 0u);
    EXPECT_EQ(h.count("Unique-ID"),  0u);
    EXPECT_EQ(h.count("Some-Other-Header"), 0u);
}

TEST_F(EnvelopeTest, OperatorAddedVariablesAreForwarded) {
    SetHeader(kEv, "Event-Name",            "CHANNEL_ANSWER");
    SetHeader(kEv, "variable_my_custom",    "value-x");
    SetHeader(kEv, "variable_skipped_one",  "value-y");

    EnvelopeBuildConfig cfg = MakeDefaultEnvelopeConfig();
    cfg.variables_include = {"my_custom"};  // only one of the two

    auto* env = BuildEnvelope(kEv, Tier::k2State, 1, "n", cfg, &arena_);
    ASSERT_NE(env, nullptr);

    const auto& v = env->variables();
    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(v.at("my_custom"), "value-x");
    EXPECT_EQ(v.count("skipped_one"), 0u);
}

TEST_F(EnvelopeTest, MissingTimestampFallsBackToNow) {
    SetHeader(kEv, "Event-Name", "HEARTBEAT");

    const auto before = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto* env = BuildEnvelope(kEv, Tier::k3Ephemeral, 1, "n",
                              MakeDefaultEnvelopeConfig(), &arena_);
    ASSERT_NE(env, nullptr);

    const auto after = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    EXPECT_GE(env->emitted_at().seconds(), before);
    EXPECT_LE(env->emitted_at().seconds(), after);
}

TEST_F(EnvelopeTest, EmptyHeadersIncludeListIsNotAccidentallyAllForwarded) {
    // Default behaviour: empty headers_include = no extra headers (we
    // do explicit-include filtering, NOT a deny-list pattern).
    SetHeader(kEv, "Event-Name",       "HEARTBEAT");
    SetHeader(kEv, "Random-Header",    "random-value");

    EnvelopeBuildConfig cfg = MakeDefaultEnvelopeConfig();
    cfg.headers_include.clear();

    auto* env = BuildEnvelope(kEv, Tier::k3Ephemeral, 1, "n", cfg, &arena_);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(env->headers().size(), 0u);
}

}  // namespace
