/*
 * src/events/tier.cc
 *
 * Implementation of osw::events::TierClassifier.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/tier.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace osw::events {

std::string_view ToString(Tier t) noexcept {
    switch (t) {
        case Tier::k1Critical:  return "tier1";
        case Tier::k2State:     return "tier2";
        case Tier::k3Ephemeral: return "tier3";
        case Tier::kUnspecified:
        default:                return "unspecified";
    }
}

namespace {

// W2-default Tier-1 events. See header comment for rationale.
constexpr const char* kDefaultTier1Events[] = {
    "CHANNEL_HANGUP_COMPLETE",
    "CHANNEL_BRIDGE",
    "CHANNEL_UNBRIDGE",
    "CDR_REPORT",
    "RECORD_START",
    "RECORD_STOP",
};

constexpr const char* kDefaultTier2Events[] = {
    "CHANNEL_CREATE",
    "CHANNEL_DESTROY",
    "CHANNEL_ANSWER",
    "CHANNEL_PROGRESS",
    "CHANNEL_CALLSTATE",
    "CHANNEL_HOLD",
    "CHANNEL_UNHOLD",
    "DTMF",
};

constexpr const char* kDefaultTier3Events[] = {
    "HEARTBEAT",
    "RE_SCHEDULE",
    "SESSION_HEARTBEAT",
    "MEDIA_BUG_START",
    "MEDIA_BUG_STOP",
    "MESSAGE",
    "CALL_UPDATE",
    "LOG",
};

}  // namespace

TierRules MakeDefaultRules() {
    TierRules r;
    for (const char* n : kDefaultTier1Events) {
        r.events.emplace(n, Tier::k1Critical);
    }
    for (const char* n : kDefaultTier2Events) {
        r.events.emplace(n, Tier::k2State);
    }
    for (const char* n : kDefaultTier3Events) {
        r.events.emplace(n, Tier::k3Ephemeral);
    }
    // Subclass globs:
    //   osw.audit.*   → Tier 1 (module's own audit channel)
    //   sofia::register, sofia::unregister → Tier 2 (state events)
    r.subclass_globs.emplace_back("osw.audit.*",      Tier::k1Critical);
    r.subclass_globs.emplace_back("sofia::register",  Tier::k2State);
    r.subclass_globs.emplace_back("sofia::unregister", Tier::k2State);
    r.default_tier = Tier::k3Ephemeral;
    return r;
}

TierClassifier::TierClassifier(TierRules rules) : rules_(std::move(rules)) {
    compiled_globs_.reserve(rules_.subclass_globs.size());
    for (const auto& [src, tier] : rules_.subclass_globs) {
        CompiledGlob g;
        g.tier = tier;
        if (!src.empty() && src.back() == '*') {
            g.prefix       = src.substr(0, src.size() - 1);
            g.has_wildcard = true;
        } else {
            g.prefix       = src;
            g.has_wildcard = false;
        }
        compiled_globs_.push_back(std::move(g));
    }
}

Tier TierClassifier::Classify(std::string_view event_name,
                              std::string_view subclass_name) const noexcept {
    // 1. Subclass globs win over name-only matches when subclass_name is
    //    non-empty — CUSTOM events route by subclass, not by the literal
    //    "CUSTOM" string.
    if (!subclass_name.empty()) {
        for (const auto& g : compiled_globs_) {
            if (g.has_wildcard) {
                if (subclass_name.size() >= g.prefix.size() &&
                    subclass_name.compare(0, g.prefix.size(), g.prefix) == 0) {
                    return g.tier;
                }
            } else {
                if (subclass_name == g.prefix) {
                    return g.tier;
                }
            }
        }
    }

    // 2. Exact event-name lookup.
    // unordered_map::find requires a heterogeneous lookup in C++20 (which
    // we have) only with custom hash/equal types; the simplest, portable
    // path is to construct a std::string once for the lookup. event_name
    // is small (≤ 24 chars in practice) and the lookup runs once per
    // event — the allocation is cheaper than retro-fitting a hetero hash.
    const std::string key(event_name);
    if (auto it = rules_.events.find(key); it != rules_.events.end()) {
        return it->second;
    }

    // 3. Fallback to default tier.
    return rules_.default_tier;
}

}  // namespace osw::events
