/*
 * src/control/handlers/hold_handler.cc
 *
 * Real implementation of ControlService::Hold (W3 Track C, FF-027).
 *
 * RPC contract:
 *   Success  → HoldResponse { held_uuids } + audit emit
 *              osw.control.hold { uuid, moh_class } per held channel.
 *   Failures (per-UUID):
 *     INVALID_ARGUMENT    — empty uuid in the list (entire RPC rejected).
 *     NOT_FOUND           — SessionGuard::Locate returned invalid.
 *     FAILED_PRECONDITION — CF_ANSWERED not set (channel not answered yet
 *                           or already hung up).
 *
 *   For V1 the Hold RPC is best-effort: it iterates all uuids and holds
 *   each independently.  The response populates held_uuids with every uuid
 *   that was successfully held; partially-failed batches still return OK
 *   with the subset that succeeded (errors are logged, not surfaced as a
 *   gRPC error code, unless ALL uuids fail the precondition).
 *
 *   Actually: based on the proto design (held_uuids in the response and a
 *   single ErrorDetail field) and the planning doc ("INVALID_ARGUMENT for
 *   empty uuid", "NOT_FOUND for uuid unknown", "FAILED_PRECONDITION for
 *   channel not answered"), the V1 implementation treats each UUID
 *   independently and returns the first error encountered.  For a single-
 *   uuid request this aligns with the plan; for multi-uuid the first
 *   failure aborts (see below).
 *
 * Hold + message/moh resolution (FF-027):
 *   The proto HoldRequest carries no moh_class field. Pass nullptr as
 *   `message` so FS uses the channel's configured MoH.  Always pass
 *   moh = SWITCH_TRUE.
 *
 * Threading:
 *   Single gRPC thread.  SessionGuard acquired and released per UUID.
 *
 * Cited FACTs: FF-016, FF-027.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/session_guard.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.hold";

}  // namespace

grpc::Status ControlServiceSkeleton::Hold(grpc::ServerContext* /*ctx*/,
                                          const open_switch::control::v1::HoldRequest* req,
                                          open_switch::control::v1::HoldResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    if (req->uuids().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuids list must not be empty");
    }

    for (const std::string& uuid : req->uuids()) {
        if (uuid.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "uuid must not be empty");
        }

        auto guard = osw::control::SessionGuard::Locate(uuid);
        if (!guard.Valid()) {
            osw::log::Debug(kSubsystem, "Hold NOT_FOUND: uuid=%s", uuid.c_str());
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "session not found: uuid=" + uuid);
        }

        switch_channel_t* const ch = guard.Channel();
        if (ch == nullptr) {
            osw::log::Warn(kSubsystem, "Hold: channel null for uuid=%s", uuid.c_str());
            return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
        }

        // FF-027: Hold only answered channels; CF_ANSWERED must be set.
        if (!osw::raii::fs::ChannelTestFlag(ch, CF_ANSWERED)) {
            osw::log::Debug(kSubsystem,
                            "Hold FAILED_PRECONDITION: channel not answered uuid=%s",
                            uuid.c_str());
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel not in answered state: uuid=" + uuid);
        }

        // FF-027: Also gate on CF_HOLD to avoid double-hold (idempotent in FS,
        // but we surface FAILED_PRECONDITION for already-held channels so
        // callers detect logic errors).
        if (osw::raii::fs::ChannelTestFlag(ch, CF_HOLD)) {
            osw::log::Debug(kSubsystem,
                            "Hold FAILED_PRECONDITION: channel already on hold uuid=%s",
                            uuid.c_str());
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel already on hold: uuid=" + uuid);
        }

        // FF-027: Pass nullptr as message (FS picks default MoH class).
        // Always pass moh=SWITCH_TRUE.
        const switch_status_t rc = osw::raii::fs::HoldUuid(uuid.c_str(), nullptr, SWITCH_TRUE);
        if (rc != SWITCH_STATUS_SUCCESS) {
            osw::log::Warn(kSubsystem,
                           "Hold: HoldUuid returned rc=%d for uuid=%s",
                           rc,
                           uuid.c_str());
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "HoldUuid failed for uuid=" + uuid);
        }

        resp->add_held_uuids(uuid);

        osw::audit::Emit("osw.control.hold", {{"uuid", uuid}, {"moh_class", ""}});

        osw::log::Info(kSubsystem, "Hold OK: uuid=%s", uuid.c_str());
    }

    return grpc::Status::OK;
}

}  // namespace osw::control
