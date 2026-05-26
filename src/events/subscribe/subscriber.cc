/*
 * src/events/subscribe/subscriber.cc
 *
 * Implementation of osw::events::Subscriber + KickReason helpers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/subscribe/subscriber.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osw::events {

std::string_view ToString(KickReason r) noexcept {
    switch (r) {
        case KickReason::kQueueFull:
            return "queue_full";
        case KickReason::kReplayEvicted:
            return "replay_evicted";
        case KickReason::kShutdown:
            return "shutdown";
        case KickReason::kClientCancelled:
            return "client_cancelled";
        case KickReason::kNone:
        default:
            return "none";
    }
}

Subscriber::Subscriber(std::string subscriber_id,
                       SubscriberFilter filter,
                       std::size_t send_queue_capacity)
    : id_(std::move(subscriber_id)), filter_(std::move(filter)), send_queue_(send_queue_capacity) {}

bool Subscriber::IsClosed() const noexcept {
    return close_flag_.load(std::memory_order_acquire) || send_queue_.IsClosed();
}

void Subscriber::RequestClose(KickReason reason) noexcept {
    // First-writer-wins on the kick reason. compare_exchange_strong
    // succeeds only if the slot is still kNone.
    int expected = static_cast<int>(KickReason::kNone);
    kick_reason_.compare_exchange_strong(
        expected, static_cast<int>(reason), std::memory_order_acq_rel, std::memory_order_acquire);

    close_flag_.store(true, std::memory_order_release);
    send_queue_.Close();
}

KickReason Subscriber::GetKickReason() const noexcept {
    return static_cast<KickReason>(kick_reason_.load(std::memory_order_acquire));
}

namespace {

// Prefix-wildcard match: `pat` ending in `*` matches any string with
// that prefix; otherwise an exact-equality match. Shared by the
// event_name and subclass_name predicates so behaviour stays
// symmetric.
[[nodiscard]] bool MatchPrefixGlob(std::string_view pat, std::string_view s) noexcept {
    if (!pat.empty() && pat.back() == '*') {
        const std::string_view prefix = pat.substr(0, pat.size() - 1);
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }
    return s == pat;
}

[[nodiscard]] bool MatchAnyPattern(const std::vector<std::string>& patterns,
                                   std::string_view s) noexcept {
    for (const auto& g : patterns) {
        if (MatchPrefixGlob(g, s))
            return true;
    }
    return false;
}

}  // namespace

bool Subscriber::MatchesFilter(Tier tier,
                               std::string_view event_name,
                               std::string_view subclass_name,
                               std::string_view node_id) const noexcept {
    // Tier filter.
    if (!filter_.tiers.empty() && filter_.tiers.count(tier) == 0) {
        return false;
    }
    // Node filter.
    if (!filter_.node_id.empty() && node_id != filter_.node_id) {
        return false;
    }
    // Event-name filter (prefix-glob). Empty = match all event names.
    if (!filter_.event_name_globs.empty() &&
        !MatchAnyPattern(filter_.event_name_globs, event_name)) {
        return false;
    }
    // Subclass filter (Gemini W2.5 C-2). Empty = match all subclasses.
    // For non-CUSTOM events `subclass_name` is empty; the same
    // MatchPrefixGlob handles that uniformly (empty-string equality or
    // any `*`-suffix glob with empty prefix matches).
    if (!filter_.subclass_globs.empty() &&
        !MatchAnyPattern(filter_.subclass_globs, subclass_name)) {
        return false;
    }
    return true;
}

}  // namespace osw::events
