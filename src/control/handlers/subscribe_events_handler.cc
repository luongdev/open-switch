/*
 * src/control/handlers/subscribe_events_handler.cc
 *
 * Real implementation of ControlService::SubscribeEvents.
 *
 * Flow:
 *   1. Refuse the stream if the module is draining or the event-plane
 *      bridges were never injected (pre-W2 builds, or a test harness
 *      that didn't call SetEventPlane).
 *   2. Cap the active stream count at config.max_subscribers; further
 *      streams are rejected with RESOURCE_EXHAUSTED.
 *   3. Parse the SubscribeEventsRequest into a SubscriberFilter:
 *      - `tiers` strings ("TIER_1_CRITICAL" / "1" / "tier1" / "1critical")
 *        are tolerated.
 *      - `event_names` are forwarded verbatim (single trailing '*'
 *        wildcard; literal otherwise) — Subscriber::MatchesFilter
 *        implements the match.
 *      - `node_id` carries through.
 *   4. Construct a Subscriber with a UUIDv7 id + the operator-configured
 *      send-queue capacity.
 *   5. Compute the replay window:
 *      - If since_seq == 0: no replay, live-tail only.
 *      - If since_seq > 0:
 *          * For EACH requested tier (or all three if `tiers` is empty),
 *            call Ring::SnapshotFromSeq(since_seq). If ANY requested
 *            tier reports found_in_window=false, the replay point was
 *            evicted → return RESOURCE_EXHAUSTED.
 *          * Pre-load the entries from each tier's snapshot into the
 *            subscriber's send queue (filter-matched via the same
 *            ExtractRoutingFields+MatchesFilter the broadcaster uses
 *            on the live-tail path — Codex W2 C-1), interleaving by
 *            ascending seq within tier (each ring is monotonic). The
 *            client sees the replay in (tier, seq) groups; documented
 *            in the proto.
 *   6. AddSubscriberAtomic to the broadcaster (Codex W2 B-1). The
 *      broadcaster holds its roster lock across the replay closure;
 *      per-tier workers block at RosterSnapshot for the duration, so
 *      no live event with seq > snapshot.max_seq is dispatched without
 *      the new subscriber visible in the roster. After
 *      AddSubscriberAtomic returns, live tail flows to the new sub.
 *      Audit-emit osw.audit.subscriber_connected.
 *   7. Writer loop:
 *      - WaitAndPop one envelope-bytes shared_ptr.
 *      - ParseFromString into an arena-owned EventEnvelope (no copy of
 *        the bytes; just a parse).
 *      - ServerWriter::Write — if false, the client cancelled or the
 *        peer broke; RequestClose(kClientCancelled).
 *      - Check ctx->IsCancelled() between iterations (gRPC stops the
 *        stream as soon as the peer disconnects; we propagate that
 *        back into the broadcaster's roster on next remove).
 *   8. On loop exit:
 *      - RemoveSubscriber from the broadcaster.
 *      - Audit-emit osw.audit.subscriber_disconnected /
 *        osw.audit.subscriber_kicked.
 *      - Map KickReason → grpc::Status (kQueueFull / kReplayEvicted →
 *        RESOURCE_EXHAUSTED; kShutdown → OK; kClientCancelled → OK;
 *        kNone → OK).
 *
 * Threading:
 *   The handler thread IS the writer thread. The broadcaster pushes
 *   into the SendQueue from its per-tier worker; this thread pops and
 *   writes. The Subscriber object stays alive via shared_ptr — the
 *   broadcaster's roster holds one ref, and we hold one locally for
 *   the RPC's duration.
 *
 * Memory:
 *   - The serialised envelope bytes are zero-copied via shared_ptr.
 *   - We parse into a thread-local-arena EventEnvelope per Write call;
 *     the arena is reset between iterations (single allocation per
 *     event). On Write success the proto is dead anyway.
 *   - No raw new/delete/malloc/free.
 *
 * Exception boundary:
 *   The full body sits inside try/catch — any std::bad_alloc from the
 *   parse path is caught, logged, and translated to grpc::Status with
 *   code INTERNAL.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/arena.h>
#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/events/v1/events.pb.h"

#include "osw/events/binder.h"
#include "osw/events/ring.h"
#include "osw/events/subscribe/broadcaster.h"
#include "osw/events/subscribe/routing.h"
#include "osw/events/subscribe/subscriber.h"
#include "osw/events/tier.h"
#include "osw/observability/audit.h"
#include "osw/observability/health.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.subscribe";

// How long to block in SendQueue::WaitAndPop per iteration. Short
// enough that the handler observes ctx->IsCancelled() promptly; long
// enough that the queue's notify_one is overwhelmingly the wake path.
constexpr auto kPollTimeout = std::chrono::milliseconds(200);

// Generate a stable, unique-enough subscriber ID. We don't need the
// strong uniqueness of UUIDv7 (which lives in osw_events_fs and pulls
// in <switch.h> transitively); collision risk is essentially zero with
// a per-process atomic counter + nanosecond timestamp + thread id.
[[nodiscard]] std::string GenerateSubscriberId() {
    static std::atomic<std::uint64_t> seq{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto counter = seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << "sub-" << ns << "-" << std::this_thread::get_id() << "-" << counter;
    return oss.str();
}

[[nodiscard]] std::optional<osw::events::Tier> ParseTierString(std::string_view s) noexcept {
    // Tolerated forms (case-insensitive on the leading token):
    //   "TIER_1_CRITICAL", "TIER_2_STATE", "TIER_3_EPHEMERAL"
    //   "1", "2", "3"
    //   "tier1", "tier2", "tier3"
    using osw::events::Tier;
    if (s == "1" || s == "tier1" || s == "TIER_1_CRITICAL" || s == "tier_1_critical")
        return Tier::k1Critical;
    if (s == "2" || s == "tier2" || s == "TIER_2_STATE" || s == "tier_2_state")
        return Tier::k2State;
    if (s == "3" || s == "tier3" || s == "TIER_3_EPHEMERAL" || s == "tier_3_ephemeral")
        return Tier::k3Ephemeral;
    return std::nullopt;
}

// Build a SubscriberFilter from the request. Codex W2 review I-1:
// if the client supplied at least one `tiers` entry but EVERY one
// failed to parse, fail INVALID_ARGUMENT rather than silently
// upgrading them to "all tiers" — that defaulted to the entire event
// stream, which surprised operators (a client asking for one tier
// would unexpectedly receive everything). If `tiers` is empty the
// client opted into all-tiers and we keep the default.
struct FilterResult {
    osw::events::SubscriberFilter filter;
    std::vector<osw::events::Tier> replay_tiers;  // resolved tiers to scan for replay
    bool ok = true;                               // false → INVALID_ARGUMENT
    std::string error;                            // non-empty when !ok
};

[[nodiscard]] FilterResult BuildFilter(
    const open_switch::control::v1::SubscribeEventsRequest& req) {
    FilterResult out;
    const bool tiers_requested = req.tiers_size() > 0;
    for (const auto& t_str : req.tiers()) {
        if (auto t = ParseTierString(t_str); t.has_value()) {
            out.filter.tiers.insert(*t);
            out.replay_tiers.push_back(*t);
        } else {
            osw::log::Warn(kSubsystem, "ignoring unparseable tier '%s'", t_str.c_str());
        }
    }
    if (tiers_requested && out.replay_tiers.empty()) {
        // Every requested tier failed to parse — fail-loud rather
        // than silently defaulting to all-tiers.
        out.ok = false;
        out.error =
            "no valid tier in `tiers`; allowed values: "
            "TIER_1_CRITICAL/TIER_2_STATE/TIER_3_EPHEMERAL "
            "(also 1/2/3 or tier1/tier2/tier3)";
        return out;
    }
    if (out.replay_tiers.empty()) {
        // Empty `tiers` → operator opted in to all tiers.
        out.replay_tiers = {osw::events::Tier::k1Critical,
                            osw::events::Tier::k2State,
                            osw::events::Tier::k3Ephemeral};
    }
    for (const auto& g : req.event_names()) {
        out.filter.event_name_globs.push_back(g);
    }
    // Gemini W2.5 C-2: plumb the subclass_globs from the proto through
    // to the runtime filter. Same prefix-wildcard semantics as
    // event_names (a trailing `*` is a prefix glob). Empty list = match
    // all subclasses (the most common case).
    for (const auto& g : req.subclass_globs()) {
        out.filter.subclass_globs.push_back(g);
    }
    out.filter.node_id = req.node_id();
    return out;
}

// Map a KickReason to a grpc::Status. Kicks that mean "client violated
// the streaming contract" map to RESOURCE_EXHAUSTED. Kicks that mean
// "the server is going away" map to OK (the client should reconnect).
[[nodiscard]] grpc::Status KickReasonToStatus(osw::events::KickReason r, std::string_view detail) {
    using osw::events::KickReason;
    switch (r) {
        case KickReason::kQueueFull:
            return grpc::Status(
                grpc::StatusCode::RESOURCE_EXHAUSTED,
                std::string("subscriber kicked: queue full (") + std::string(detail) + ")");
        case KickReason::kReplayEvicted:
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "since_seq outside replay window");
        case KickReason::kShutdown:
            // Clean shutdown — OK status, client retries.
            return grpc::Status::OK;
        case KickReason::kClientCancelled:
            return grpc::Status::OK;
        case KickReason::kNone:
        default:
            return grpc::Status::OK;
    }
}

// Replay since_seq from every requested tier. Pushes filter-matched
// entries into the subscriber's send queue. Returns true on success;
// false if ANY tier reported the replay point as evicted (caller
// returns RESOURCE_EXHAUSTED).
[[nodiscard]] bool ReplaySinceSeq(osw::events::RingSet* rings,
                                  const std::vector<osw::events::Tier>& replay_tiers,
                                  std::uint64_t since_seq,
                                  osw::events::Subscriber& sub) {
    if (rings == nullptr)
        return true;  // no rings means no replay — fall through to live tail
    for (const auto t : replay_tiers) {
        osw::events::Ring* ring = rings->Get(t);
        if (ring == nullptr)
            continue;
        const auto snap = ring->SnapshotFromSeq(since_seq);
        if (!snap.found_in_window) {
            osw::log::Warn(kSubsystem,
                           "since_seq=%llu evicted on tier=%s (min=%llu max=%llu)",
                           static_cast<unsigned long long>(since_seq),
                           std::string(osw::events::ToString(t)).c_str(),
                           static_cast<unsigned long long>(snap.current_min_seq),
                           static_cast<unsigned long long>(snap.current_max_seq));
            sub.RequestClose(osw::events::KickReason::kReplayEvicted);
            return false;
        }
        for (const auto& entry : snap.entries) {
            if (!entry.envelope_bytes)
                continue;
            // Codex W2 C-1: apply the subscriber's filter to replay
            // entries the same way the broadcaster does for live tail.
            // The routing-fields scanner is a lightweight manual wire-
            // format walk (osw/events/subscribe/routing.h); cheap enough
            // to run per replay entry. The effective tier is the ring
            // we're scanning (entries in tier-N's ring are by definition
            // tier-N, even if the embedded proto field disagrees).
            const auto rf = osw::events::ExtractRoutingFields(*entry.envelope_bytes);
            const osw::events::Tier effective_tier =
                (t == osw::events::Tier::kUnspecified) ? rf.tier : t;
            if (!sub.MatchesFilter(effective_tier, rf.event_name, rf.subclass_name, rf.node_id)) {
                continue;
            }
            if (!sub.Queue().TryPush(entry.envelope_bytes)) {
                // Replay overflow → kick before the broadcaster ever
                // gets to push live entries. send_queue_capacity should
                // be configured > ring capacity to avoid this in
                // practice.
                osw::log::Warn(kSubsystem,
                               "subscriber id=%s replay overflow on tier=%s; "
                               "kicking with queue_full",
                               sub.Id().c_str(),
                               std::string(osw::events::ToString(t)).c_str());
                sub.RequestClose(osw::events::KickReason::kQueueFull);
                return false;
            }
        }
    }
    return true;
}

// Writer loop: pop bytes from the queue, parse + write to the gRPC
// stream. Exits when the subscriber is closed, the queue closes empty,
// or ctx->IsCancelled() becomes true (gRPC observed peer disconnect).
void WriterLoop(grpc::ServerContext* ctx,
                osw::events::Subscriber& sub,
                grpc::ServerWriter<open_switch::events::v1::EventEnvelope>* writer) {
    google::protobuf::Arena arena;
    while (true) {
        if (ctx != nullptr && ctx->IsCancelled()) {
            sub.RequestClose(osw::events::KickReason::kClientCancelled);
            return;
        }
        auto maybe_bytes = sub.Queue().WaitAndPop(kPollTimeout);
        if (!maybe_bytes.has_value()) {
            // Timeout-empty OR closed-empty. Distinguish via IsClosed.
            if (sub.IsClosed())
                return;
            continue;
        }
        auto bytes = std::move(*maybe_bytes);
        if (!bytes)
            continue;
        arena.Reset();
        // Arena::Create<T> replaces the deprecated CreateMessage<T> in
        // protobuf 4.x; the protoc generated headers shipped with the
        // FS builder image expose the new API only.
        auto* env = google::protobuf::Arena::Create<open_switch::events::v1::EventEnvelope>(&arena);
        if (!env->ParseFromString(*bytes)) {
            osw::log::Warn(kSubsystem,
                           "subscriber id=%s: failed to parse envelope bytes (size=%zu); "
                           "dropping",
                           sub.Id().c_str(),
                           bytes->size());
            continue;
        }
        if (!writer->Write(*env)) {
            // gRPC writer reports false on peer-side cancellation.
            sub.RequestClose(osw::events::KickReason::kClientCancelled);
            return;
        }
    }
}

}  // namespace

grpc::Status ControlServiceSkeleton::SubscribeEvents(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::SubscribeEventsRequest* req,
    grpc::ServerWriter<open_switch::events::v1::EventEnvelope>* writer) {
    try {
        if (req == nullptr || writer == nullptr) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "null request or writer");
        }
        // Codex W2 C-3: acquire-load the event-plane bridges. The
        // store side in SetEventPlane uses release-stores so a
        // non-null broadcaster_ implies rings_ and the caps are
        // visible to this thread. We snapshot all four into locals
        // so subsequent reads in this RPC see a consistent set even
        // if SetEventPlane is called again concurrently (it's not in
        // practice, but the atomic discipline is cheap).
        osw::events::Broadcaster* const bcast = broadcaster_.load(std::memory_order_acquire);
        if (bcast == nullptr) {
            // Pre-W2 build or test harness that didn't call SetEventPlane.
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "event plane not initialised");
        }
        osw::events::RingSet* const rings = rings_.load(std::memory_order_acquire);
        const std::uint32_t max_subscribers = max_subscribers_.load(std::memory_order_acquire);
        const std::uint32_t queue_capacity =
            subscriber_send_queue_capacity_.load(std::memory_order_acquire);

        // Cap on active subscribers. The roster_size check is racy by
        // design (we might briefly exceed the cap by 1 under high arrival
        // rate); the next subscriber gets rejected promptly.
        if (max_subscribers > 0 && bcast->SubscriberCount() >= max_subscribers) {
            osw::log::Warn(
                kSubsystem, "rejecting SubscribeEvents: at max_subscribers=%u", max_subscribers);
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "at max_subscribers cap");
        }

        // Build filter and resolve the per-tier replay set. I-1: all
        // unparseable tier strings → INVALID_ARGUMENT.
        const FilterResult fr = BuildFilter(*req);
        if (!fr.ok) {
            osw::log::Warn(kSubsystem, "rejecting SubscribeEvents: %s", fr.error.c_str());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, fr.error);
        }

        // Create the subscriber with a unique-enough id.
        const std::string sub_id = GenerateSubscriberId();
        const std::size_t cap = queue_capacity == 0 ? 4096 : queue_capacity;
        auto sub = std::make_shared<osw::events::Subscriber>(sub_id, fr.filter, cap);

        // Atomic snapshot+add (Codex W2 B-1). The broadcaster holds
        // its roster lock across the replay closure, so per-tier
        // worker threads block at RosterSnapshot() and any events
        // they have already popped from the ring sit in their batch
        // vector until we release. When workers proceed, the new
        // subscriber is in the roster and receives those popped
        // events — closing the replay→live-tail seq gap.
        //
        // Worst case: the worker dispatches an entry that was ALSO
        // in our snapshot (and which we already pushed) — clients
        // dedup on `seq` per (node, tier), as documented in
        // SubscribeEventsRequest.since_seq.
        bool replay_ok = true;
        bcast->AddSubscriberAtomic(sub, [&](osw::events::Subscriber& s) {
            if (req->since_seq() > 0) {
                if (!ReplaySinceSeq(rings, fr.replay_tiers, req->since_seq(), s)) {
                    replay_ok = false;
                }
            }
        });
        if (!replay_ok) {
            // ReplaySinceSeq already called sub.RequestClose() and set
            // a kick reason. The subscriber is now in the roster but
            // closed; we Remove + return the matching status. The
            // broadcaster's IsClosed gate prevents any further push to
            // its SendQueue.
            bcast->RemoveSubscriber(sub_id);
            return KickReasonToStatus(sub->GetKickReason(), "during since_seq replay");
        }
        osw::audit::Emit(
            "subscriber_connected",
            {{"subscriber_id", sub_id}, {"since_seq", std::to_string(req->since_seq())}});
        osw::log::Info(kSubsystem,
                       "subscriber id=%s connected (tiers=%zu globs=%zu "
                       "node_id='%s' since_seq=%llu)",
                       sub_id.c_str(),
                       fr.filter.tiers.size(),
                       fr.filter.event_name_globs.size(),
                       fr.filter.node_id.c_str(),
                       static_cast<unsigned long long>(req->since_seq()));

        // Drive the stream. Returns when the queue closes or the peer
        // disconnects.
        WriterLoop(ctx, *sub, writer);

        // Unwind.
        const auto reason = sub->GetKickReason();
        bcast->RemoveSubscriber(sub_id);

        if (reason == osw::events::KickReason::kQueueFull ||
            reason == osw::events::KickReason::kReplayEvicted) {
            osw::audit::Emit("subscriber_kicked",
                             {{"subscriber_id", sub_id},
                              {"reason", std::string(osw::events::ToString(reason))}});
        } else {
            osw::audit::Emit("subscriber_disconnected",
                             {{"subscriber_id", sub_id},
                              {"reason", std::string(osw::events::ToString(reason))}});
        }
        osw::log::Info(kSubsystem,
                       "subscriber id=%s disconnected (reason=%s)",
                       sub_id.c_str(),
                       std::string(osw::events::ToString(reason)).c_str());

        return KickReasonToStatus(reason, "writer-loop exit");
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "SubscribeEvents threw: %s", e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            std::string("subscribe events failed: ") + e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "SubscribeEvents threw unknown exception");
        return grpc::Status(grpc::StatusCode::INTERNAL, "subscribe events failed: unknown");
    }
}

}  // namespace osw::control
