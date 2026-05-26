/*
 * include/osw/events/subscribe/subscriber.h
 *
 * osw::events::Subscriber — per-SubscribeEvents-stream state.
 *
 * Each in-flight SubscribeEvents RPC owns one Subscriber. The
 * subscriber holds:
 *   - filter (tiers, event_name globs, node_id)
 *   - bounded SendQueue (outbox the broadcaster pushes into)
 *   - atomic close_flag (broadcaster sets on kick / handler sets on
 *     RPC exit)
 *   - kick_reason (set when the broadcaster boots the subscriber so
 *     the handler can surface the right grpc::Status)
 *
 * Why no grpc::ServerWriter* here: the writer is the consumer of the
 * SendQueue, executed by the gRPC handler thread (one thread per
 * in-flight RPC). The Subscriber object is reachable from both the
 * broadcaster thread (push side) and the gRPC handler (pop +
 * lifecycle side). Decoupling via a SendQueue keeps the contention
 * minimal and the lifetime correct.
 *
 * Lifetime:
 *   - The Broadcaster owns shared_ptr<Subscriber> in its roster.
 *   - The gRPC handler also holds a shared_ptr<Subscriber> for the
 *     duration of the RPC. When the RPC returns, the handler
 *     unregisters the subscriber from the broadcaster (which drops
 *     the broadcaster's reference) and then releases its own ref.
 *     The Subscriber destructs when both refs are released — which
 *     happens AFTER the writer thread has exited (the handler thread
 *     IS the writer thread).
 *
 * Threading:
 *   - filter_ and node_id_ are read-only after construction.
 *   - send_queue_ is internally thread-safe.
 *   - close_flag_ + kick_reason_ are atomics.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_SUBSCRIBE_SUBSCRIBER_H_
#define OSW_EVENTS_SUBSCRIBE_SUBSCRIBER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "osw/events/subscribe/send_queue.h"
#include "osw/events/tier.h"

namespace osw::events {

/// Reason a subscriber was kicked. The handler maps this to a
/// grpc::Status when it exits.
enum class KickReason : int {
    kNone = 0,
    kQueueFull = 1,        // RESOURCE_EXHAUSTED with "queue full"
    kReplayEvicted = 2,    // since_seq outside ring window
    kShutdown = 3,         // module is draining
    kClientCancelled = 4,  // gRPC writer saw a Write() failure
};

[[nodiscard]] std::string_view ToString(KickReason r) noexcept;

/// Filter specification supplied by the subscriber.
struct SubscriberFilter {
    /// Tiers the subscriber wants. Empty = all tiers.
    std::unordered_set<Tier> tiers;

    /// Event-name globs. Empty = match all event names. V1 supports
    /// prefix-wildcard only (Codex W2.5 C-2): a trailing `*` matches
    /// any suffix (`foo*` → `foobar`); any other input is an exact
    /// match (`foo` → `foo` only). Generic glob (`*` anywhere, `?`,
    /// `[abc]`) is NOT supported.
    std::vector<std::string> event_name_globs;

    /// Event-subclass globs matched against the `Event-Subclass` header
    /// carried on `CUSTOM` events. Empty = match all subclasses (the
    /// subscription is unfiltered on the subclass axis). Same
    /// prefix-wildcard semantics as `event_name_globs`: a trailing `*`
    /// is a prefix match (`osw.audit.*`), anything else is an exact
    /// match. Closes Gemini W2.5 review item C-2 — the W2.5 sprint
    /// only updated docs and never implemented the actual filter.
    ///
    /// Non-CUSTOM events have an empty subclass; if `subclass_globs`
    /// is set, an envelope with an empty subclass matches only when
    /// the empty string `""` (or an exact-empty pattern) appears in
    /// the list.
    std::vector<std::string> subclass_globs;

    /// Node filter. Empty = any node. When non-empty, the envelope's
    /// node_id must equal this string to match.
    std::string node_id;
};

class Subscriber {
  public:
    Subscriber(std::string subscriber_id, SubscriberFilter filter, std::size_t send_queue_capacity);

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    /// Stable identifier (UUIDv7) the handler generates at registration.
    /// Used in logs + osw.audit.subscriber_* event headers.
    [[nodiscard]] const std::string& Id() const noexcept { return id_; }

    [[nodiscard]] const SubscriberFilter& Filter() const noexcept { return filter_; }

    SendQueue& Queue() noexcept { return send_queue_; }
    const SendQueue& Queue() const noexcept { return send_queue_; }

    /// True iff (a) the broadcaster (or anyone) called RequestClose(),
    /// OR (b) the underlying send queue is closed.
    [[nodiscard]] bool IsClosed() const noexcept;

    /// Flip the close flag + (optionally) the kick reason. The writer
    /// thread checks IsClosed() after each WaitAndPop and exits the
    /// RPC; the broadcaster also stops pushing into the queue (it
    /// checks IsClosed before TryPush). Idempotent — the FIRST caller
    /// wins for kick_reason (we keep the earliest reason).
    void RequestClose(KickReason reason) noexcept;

    [[nodiscard]] KickReason GetKickReason() const noexcept;

    /// Returns true if `envelope` matches the subscriber's filter.
    /// All string views are borrowed by the caller; the function only
    /// reads them. `subclass_name` is empty for non-CUSTOM events.
    [[nodiscard]] bool MatchesFilter(Tier tier,
                                     std::string_view event_name,
                                     std::string_view subclass_name,
                                     std::string_view node_id) const noexcept;

  private:
    const std::string id_;
    const SubscriberFilter filter_;
    SendQueue send_queue_;
    std::atomic<bool> close_flag_{false};
    std::atomic<int> kick_reason_{static_cast<int>(KickReason::kNone)};
};

}  // namespace osw::events

#endif  // OSW_EVENTS_SUBSCRIBE_SUBSCRIBER_H_
