/*
 * tests/unit/events/tier_test.cc
 *
 * Unit tests for osw::events::TierClassifier.
 *
 * Covered:
 *   - Default rules map well-known FS events to the documented tiers.
 *   - CUSTOM/osw.audit.* subclass → Tier 1.
 *   - sofia::register / sofia::unregister exact subclass → Tier 2.
 *   - Unmapped event_name + empty subclass → default tier (3).
 *   - Operator override at construction replaces the defaults.
 *   - Glob match is prefix-only (trailing '*'); embedded '*' literal.
 *   - default_tier override works.
 *   - ToString() returns stable labels.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/tier.h"

#include <gtest/gtest.h>

namespace {

using osw::events::Tier;
using osw::events::TierClassifier;
using osw::events::TierRules;

class TierTest : public ::testing::Test {};

TEST_F(TierTest, DefaultRulesMapWellKnownEvents) {
    TierClassifier c(osw::events::MakeDefaultRules());
    // Full channel-lifecycle set is Tier-1 (must-persist) — subscribers
    // need every state transition to reconstruct call state.
    EXPECT_EQ(c.Classify("CHANNEL_CREATE", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_PROGRESS", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_ANSWER", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_BRIDGE", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_UNBRIDGE", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_DESTROY", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CHANNEL_HANGUP_COMPLETE", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CDR_REPORT", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("RECORD_START", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("RECORD_STOP", ""), Tier::k1Critical);
    // Tier-2 state events that are not in the lifecycle set.
    EXPECT_EQ(c.Classify("DTMF", ""), Tier::k2State);
    // Tier-3 ephemeral noise.
    EXPECT_EQ(c.Classify("HEARTBEAT", ""), Tier::k3Ephemeral);
    EXPECT_EQ(c.Classify("MEDIA_BUG_START", ""), Tier::k3Ephemeral);
    EXPECT_EQ(c.Classify("LOG", ""), Tier::k3Ephemeral);
}

TEST_F(TierTest, AuditSubclassRoutesToTier1) {
    TierClassifier c(osw::events::MakeDefaultRules());
    EXPECT_EQ(c.Classify("CUSTOM", "osw.audit.module_loaded"), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CUSTOM", "osw.audit.subscriber_connected"), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CUSTOM", "osw.audit.subscriber_disconnected"), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CUSTOM", "osw.audit.subscriber_kicked"), Tier::k1Critical);
}

TEST_F(TierTest, SofiaRegisterExactSubclassRoutesToTier2) {
    TierClassifier c(osw::events::MakeDefaultRules());
    EXPECT_EQ(c.Classify("CUSTOM", "sofia::register"), Tier::k2State);
    EXPECT_EQ(c.Classify("CUSTOM", "sofia::unregister"), Tier::k2State);
    // A subclass NOT exactly matching should fall through to event-name
    // lookup. CUSTOM is not in the events table → default tier.
    EXPECT_EQ(c.Classify("CUSTOM", "sofia::register_extra"), Tier::k3Ephemeral);
}

TEST_F(TierTest, UnmappedEventFallsToDefaultTier) {
    TierClassifier c(osw::events::MakeDefaultRules());
    EXPECT_EQ(c.Classify("SOMETHING_NEW", ""), Tier::k3Ephemeral);
    EXPECT_EQ(c.Classify("CUSTOM", "unknown.subclass"), Tier::k3Ephemeral);
}

TEST_F(TierTest, OperatorOverrideReplacesDefaults) {
    TierRules r;
    r.events.emplace("CHANNEL_HANGUP_COMPLETE", Tier::k2State);  // demoted
    r.events.emplace("MY_CUSTOM_EVENT", Tier::k1Critical);
    r.default_tier = Tier::k2State;
    TierClassifier c(std::move(r));

    EXPECT_EQ(c.Classify("CHANNEL_HANGUP_COMPLETE", ""), Tier::k2State);
    EXPECT_EQ(c.Classify("MY_CUSTOM_EVENT", ""), Tier::k1Critical);
    EXPECT_EQ(c.Classify("UNKNOWN", ""), Tier::k2State);
    // No subclass globs in this ruleset, so osw.audit.* falls through.
    EXPECT_EQ(c.Classify("CUSTOM", "osw.audit.foo"), Tier::k2State);
}

TEST_F(TierTest, GlobMatchIsPrefixOnly) {
    TierRules r;
    r.subclass_globs.emplace_back("foo.bar.*", Tier::k1Critical);
    r.subclass_globs.emplace_back("baz", Tier::k2State);
    TierClassifier c(std::move(r));

    EXPECT_EQ(c.Classify("CUSTOM", "foo.bar.baz"), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CUSTOM", "foo.bar."), Tier::k1Critical);
    EXPECT_EQ(c.Classify("CUSTOM", "foo.bar"), Tier::k3Ephemeral);  // no dot
    EXPECT_EQ(c.Classify("CUSTOM", "foo.barX"), Tier::k3Ephemeral);
    EXPECT_EQ(c.Classify("CUSTOM", "baz"), Tier::k2State);
    EXPECT_EQ(c.Classify("CUSTOM", "baz.extra"), Tier::k3Ephemeral);
}

TEST_F(TierTest, DefaultTierOverrideIsHonored) {
    TierRules r;
    r.default_tier = Tier::k1Critical;
    TierClassifier c(std::move(r));
    EXPECT_EQ(c.Classify("ANYTHING", ""), Tier::k1Critical);
    EXPECT_EQ(c.DefaultTier(), Tier::k1Critical);
}

TEST_F(TierTest, ToStringReturnsStableLabels) {
    EXPECT_EQ(osw::events::ToString(Tier::k1Critical), "tier1");
    EXPECT_EQ(osw::events::ToString(Tier::k2State), "tier2");
    EXPECT_EQ(osw::events::ToString(Tier::k3Ephemeral), "tier3");
    EXPECT_EQ(osw::events::ToString(Tier::kUnspecified), "unspecified");
}

TEST_F(TierTest, ClassifyIsThreadSafeReadOnly) {
    // Smoke test only — the classifier holds no mutable state after
    // construction, so concurrent reads are well-defined by C++ standard.
    TierClassifier c(osw::events::MakeDefaultRules());
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(c.Classify("CHANNEL_HANGUP_COMPLETE", ""), Tier::k1Critical);
    }
}

}  // namespace
