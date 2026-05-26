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

#include "src/control/control_service_skeleton.h"

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "osw/observability/health.h"

namespace osw::control {

namespace {

open_switch::control::v1::HealthResponse::Status MapStatus(Health::Status s) noexcept {
    using Out = open_switch::control::v1::HealthResponse;
    switch (s) {
        case Health::Status::kServing:     return Out::SERVING;
        case Health::Status::kNotServing:  return Out::NOT_SERVING;
        case Health::Status::kDraining:    return Out::DRAINING;
        case Health::Status::kUnspecified: return Out::STATUS_UNSPECIFIED;
    }
    return Out::STATUS_UNSPECIFIED;
}

}  // namespace

grpc::Status ControlServiceSkeleton::Health(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::HealthRequest* /*req*/,
    open_switch::control::v1::HealthResponse* resp) {

    if (!resp || !health_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "health aggregator not initialised");
    }

    const auto snap = health_->GetSnapshot();
    resp->set_status(MapStatus(snap.status));
    resp->set_module_version(module_version_.empty() ? snap.module_version
                                                     : module_version_);
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

ControlServiceSkeleton::ControlServiceSkeleton(osw::Health* health) noexcept
    : health_(health) {}

void ControlServiceSkeleton::SetVersions(std::string module_version,
                                         std::string freeswitch_version) {
    module_version_     = std::move(module_version);
    freeswitch_version_ = std::move(freeswitch_version);
}

}  // namespace osw::control
