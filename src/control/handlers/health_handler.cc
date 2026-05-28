/*
 * src/control/handlers/health_handler.cc
 *
 * Real implementation of ControlService::Health.
 *
 * Reads a snapshot from osw::Health and copies into the
 * HealthResponse proto. Status enum mapping mirrors the proto's
 * HealthResponse::Status definition.
 *
 * No FreeSWITCH dependency; this handler is FS-agnostic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/control/idempotency_cache.h"
#include "osw/core/config.h"
#include "osw/observability/health.h"

// Forward-declare for W6C setter implementations. Full definition in
// osw/media/bug_manager.h but that pulls in FS types we avoid here.
namespace osw::media {
class MediaBugManager;
}

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

open_switch::control::v1::HealthResponse::Status MapStatus(Health::Status s) noexcept {
    using Out = open_switch::control::v1::HealthResponse;
    switch (s) {
        case Health::Status::kServing:
            return Out::SERVING;
        case Health::Status::kNotServing:
            return Out::NOT_SERVING;
        case Health::Status::kDraining:
            return Out::DRAINING;
        case Health::Status::kUnspecified:
            return Out::STATUS_UNSPECIFIED;
    }
    return Out::STATUS_UNSPECIFIED;
}

}  // namespace

grpc::Status ControlServiceSkeleton::Health(grpc::ServerContext* /*ctx*/,
                                            const open_switch::control::v1::HealthRequest* /*req*/,
                                            open_switch::control::v1::HealthResponse* resp) {
    if (!resp || !health_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "health aggregator not initialised");
    }

    const auto snap = health_->GetSnapshot();
    resp->set_status(MapStatus(snap.status));
    resp->set_module_version(module_version_.empty() ? snap.module_version : module_version_);
    resp->set_freeswitch_version(freeswitch_version_.empty() ? snap.freeswitch_version
                                                             : freeswitch_version_);
    resp->set_active_channels(snap.active_channels);
    resp->set_active_media_bugs(snap.active_media_bugs);
    resp->set_events_emitted_total(snap.events_emitted_total);
    resp->set_subscriber_count(snap.subscriber_count);
    resp->set_tier1_ring_fill_pct(snap.tier1_ring_fill_pct);
    resp->set_tier2_ring_fill_pct(snap.tier2_ring_fill_pct);
    resp->set_tier3_ring_fill_pct(snap.tier3_ring_fill_pct);
    resp->set_tier1_dropped_total(snap.tier1_dropped_total);
    resp->set_tier2_dropped_total(snap.tier2_dropped_total);
    resp->set_tier3_dropped_total(snap.tier3_dropped_total);

    return grpc::Status::OK;
}

ControlServiceSkeleton::ControlServiceSkeleton(osw::Health* health) noexcept : health_(health) {}

void ControlServiceSkeleton::SetVersions(std::string module_version,
                                         std::string freeswitch_version) {
    module_version_ = std::move(module_version);
    freeswitch_version_ = std::move(freeswitch_version);
}

void ControlServiceSkeleton::SetIdempotencyCache(IdempotencyCache* cache) noexcept {
    idempotency_cache_.store(cache, std::memory_order_release);
}

void ControlServiceSkeleton::SetEventPlane(events::Broadcaster* broadcaster,
                                           events::RingSet* rings,
                                           std::uint32_t max_subscribers,
                                           std::uint32_t subscriber_send_queue_capacity) noexcept {
    // Codex W2 review C-3: release-stores so any SubscribeEvents
    // handler that subsequently acquires sees the new values. Order
    // the writes so handlers either see (broadcaster, rings, caps) as
    // a consistent set or all defaults — never partial. The
    // SubscribeEvents handler reads broadcaster_ first and bails on
    // null, so storing capacities first and broadcaster_ last
    // guarantees a non-null broadcaster_ implies the capacities are
    // also visible.
    max_subscribers_.store(max_subscribers, std::memory_order_relaxed);
    subscriber_send_queue_capacity_.store(subscriber_send_queue_capacity,
                                          std::memory_order_relaxed);
    rings_.store(rings, std::memory_order_release);
    broadcaster_.store(broadcaster, std::memory_order_release);
}

void ControlServiceSkeleton::SetMediaBugManager(osw::media::MediaBugManager* mgr) noexcept {
    bug_mgr_.store(mgr, std::memory_order_release);
}

void ControlServiceSkeleton::SetActiveMediaStreams(
    osw::control::ActiveMediaStreams* streams) noexcept {
    active_media_streams_.store(streams, std::memory_order_release);
}

void ControlServiceSkeleton::SetActiveBots(osw::control::ActiveBots* bots) noexcept {
    active_bots_.store(bots, std::memory_order_release);
}

void ControlServiceSkeleton::SetConfig(const osw::Config* config) noexcept {
    config_.store(config, std::memory_order_release);
}

}  // namespace osw::control
