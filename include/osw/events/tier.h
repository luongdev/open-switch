/*
 * include/osw/events/tier.h
 *
 * osw::events::Tier + classifier.
 *
 * Tier classification routes a FreeSWITCH event into one of three
 * in-memory rings:
 *
 *   Tier 1 — critical/billing-grade. Must persist; subscribers run in
 *            HA pairs. Default mappings: CHANNEL_HANGUP_COMPLETE,
 *            CHANNEL_BRIDGE, CHANNEL_UNBRIDGE, CDR_REPORT, RECORD_START,
 *            RECORD_STOP, CUSTOM/osw.audit.* (the module's own audit
 *            channel — see osw::audit).
 *
 *   Tier 2 — call-state. Important but eventually-consistent OK. Default
 *            mappings: CHANNEL_CREATE, CHANNEL_DESTROY, CHANNEL_ANSWER,
 *            CHANNEL_PROGRESS, CHANNEL_CALLSTATE, CHANNEL_HOLD,
 *            CHANNEL_UNHOLD, DTMF, CUSTOM/sofia::register,
 *            CUSTOM/sofia::unregister.
 *
 *   Tier 3 — ephemeral. Best-effort. Default mappings: HEARTBEAT,
 *            RE_SCHEDULE, SESSION_HEARTBEAT, MEDIA_BUG_START,
 *            MEDIA_BUG_STOP, MESSAGE, CALL_UPDATE, LOG, and the
 *            unmapped-event fallback.
 *
 * Operator overrides are configured via the open_switch.conf.xml
 * <tier-rules> block (parsed by W2 config-extension code, not yet
 * wired in this commit). Format:
 *
 *   <param name="tier1-events" value="CHANNEL_HANGUP_COMPLETE,CDR_REPORT"/>
 *   <param name="tier1-custom-subclasses" value="osw.audit.*,my.billing.*"/>
 *   <param name="tier2-events" value="..."/>
 *   <param name="tier3-events" value="..."/>
 *   <param name="default-tier" value="3"/>
 *
 * Glob match on subclass uses a single trailing `*` wildcard
 * (`prefix.*` matches anything starting with `prefix.`); full regex is
 * out of scope.
 *
 * This header is FS-agnostic — pure C++ on strings. Tested via
 * tests/unit/events/tier_test.cc.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_TIER_H_
#define OSW_EVENTS_TIER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osw::events {

enum class Tier : int {
    kUnspecified = 0,
    k1Critical   = 1,
    k2State      = 2,
    k3Ephemeral  = 3,
};

/// Returns a stable lowercase string for a Tier ("tier1", "tier2",
/// "tier3", "unspecified"). Used in metric labels and logs.
[[nodiscard]] std::string_view ToString(Tier t) noexcept;

/// Configuration for the classifier. Operators supply this via the
/// module XML config; the classifier compiles it into the runtime
/// matcher form at construction.
struct TierRules {
    /// Exact-match event-name → tier (e.g. "CHANNEL_HANGUP_COMPLETE" → 1).
    /// Empty defaults to the wave's documented defaults; pass an empty
    /// rules struct to MakeDefaultRules() to load them.
    std::unordered_map<std::string, Tier> events;

    /// Subclass globs → tier. A glob is a literal prefix optionally
    /// ending in `*`, e.g. "osw.audit.*" matches subclass names
    /// starting with "osw.audit.". Exact match (no trailing `*`) is
    /// supported too.
    std::vector<std::pair<std::string, Tier>> subclass_globs;

    /// Tier returned when no rule matches. Defaults to Tier 3.
    Tier default_tier = Tier::k3Ephemeral;
};

/// Returns the W2-default ruleset (Tier 1/2/3 per the header doc).
[[nodiscard]] TierRules MakeDefaultRules();

/// Stateless classifier built from a TierRules. Construction precompiles
/// the glob list into a vector<pair<prefix, tier>> for fast prefix
/// matching. The Classify method is thread-safe (the rules table is
/// read-only after construction).
class TierClassifier {
 public:
    explicit TierClassifier(TierRules rules);

    /// Classifies an (event_name, subclass_name) pair. Both strings are
    /// borrowed; they are only read for the duration of the call.
    /// `subclass_name` may be empty for non-CUSTOM events.
    [[nodiscard]] Tier Classify(std::string_view event_name,
                                std::string_view subclass_name) const noexcept;

    /// The default tier returned when no rule matches.
    [[nodiscard]] Tier DefaultTier() const noexcept { return rules_.default_tier; }

    /// Read-only access for tests + debug introspection.
    [[nodiscard]] const TierRules& Rules() const noexcept { return rules_; }

 private:
    struct CompiledGlob {
        std::string prefix;
        bool        has_wildcard = false;  // true when source ended in '*'
        Tier        tier         = Tier::k3Ephemeral;
    };

    TierRules                 rules_;
    std::vector<CompiledGlob> compiled_globs_;
};

}  // namespace osw::events

#endif  // OSW_EVENTS_TIER_H_
