/*
 * src/control/handlers/unhold_handler.cc
 *
 * Real implementation of ControlService::Unhold (W3 Track C, FF-027).
 *
 * RPC contract:
 *   Success  → UnholdResponse { unheld_uuids } + audit emit
 *              osw.control.unhold { uuid } per unheld channel.
 *   Failures (per-UUID):
 *     INVALID_ARGUMENT    — empty uuids list or empty uuid string.
 *     NOT_FOUND           — SessionGuard::Locate returned invalid.
 *     FAILED_PRECONDITION — CF_HOLD not set (channel is not on hold).
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

constexpr const char* kSubsystem = "control.unhold";

}  // namespace

grpc::Status ControlServiceSkeleton::Unhold(grpc::ServerContext* /*ctx*/,
                                            const open_switch::control::v1::UnholdRequest* req,
                                            open_switch::control::v1::UnholdResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    if (req->uuids().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuids list must not be empty");
    }

    for (const std::string& uuid : req->uuids()) {
        if (uuid.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuid must not be empty");
        }

        auto guard = osw::control::SessionGuard::Locate(uuid);
        if (!guard.Valid()) {
            osw::log::Debug(kSubsystem, "Unhold NOT_FOUND: uuid=%s", uuid.c_str());
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found: uuid=" + uuid);
        }

        switch_channel_t* const ch = guard.Channel();
        if (ch == nullptr) {
            osw::log::Warn(kSubsystem, "Unhold: channel null for uuid=%s", uuid.c_str());
            return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
        }

        // FF-027: Unhold only channels that are actually on hold.
        if (!osw::raii::fs::ChannelTestFlag(ch, CF_HOLD)) {
            osw::log::Debug(kSubsystem,
                            "Unhold FAILED_PRECONDITION: channel not on hold uuid=%s",
                            uuid.c_str());
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel is not on hold: uuid=" + uuid);
        }

        const switch_status_t rc = osw::raii::fs::UnholdUuid(uuid.c_str());
        if (rc != SWITCH_STATUS_SUCCESS) {
            osw::log::Warn(
                kSubsystem, "Unhold: UnholdUuid returned rc=%d for uuid=%s", rc, uuid.c_str());
            return grpc::Status(grpc::StatusCode::INTERNAL, "UnholdUuid failed for uuid=" + uuid);
        }

        resp->add_unheld_uuids(uuid);

        osw::audit::Emit("osw.control.unhold", {{"uuid", uuid}});

        osw::log::Info(kSubsystem, "Unhold OK: uuid=%s", uuid.c_str());
    }

    return grpc::Status::OK;
}

}  // namespace osw::control
