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

bool Subscriber::MatchesFilter(Tier tier,
                               std::string_view event_name,
                               std::string_view node_id) const noexcept {
    // Tier filter.
    if (!filter_.tiers.empty() && filter_.tiers.count(tier) == 0) {
        return false;
    }
    // Node filter.
    if (!filter_.node_id.empty() && node_id != filter_.node_id) {
        return false;
    }
    // Event-name filter (prefix-glob).
    if (filter_.event_name_globs.empty()) {
        return true;  // unfiltered = match all
    }
    for (const auto& g : filter_.event_name_globs) {
        if (!g.empty() && g.back() == '*') {
            const std::string_view prefix(g.data(), g.size() - 1);
            if (event_name.size() >= prefix.size() &&
                event_name.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        } else if (event_name == g) {
            return true;
        }
    }
    return false;
}

}  // namespace osw::events
