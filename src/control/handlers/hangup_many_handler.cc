/*
 * src/control/handlers/hangup_many_handler.cc
 *
 * Real implementation of ControlService::HangupMany (W3 Track A).
 *
 * RPC contract:
 *   Iterates the uuid list and attempts Hangup per uuid. Never
 *   short-circuits — every uuid is attempted regardless of whether
 *   prior uuids failed. Successfully hung-up UUIDs are collected
 *   into HangupManyResponse.hungup_uuids. Audit emit
 *   osw.control.hangup is fired per successfully hung-up uuid
 *   (mirrors the Hangup handler — one event per uuid).
 *
 * Error handling:
 *   Per-uuid errors are logged but do NOT fail the overall RPC.
 *   The caller infers per-uuid outcome by checking which UUIDs
 *   appear in hungup_uuids. If the request list is empty, OK is
 *   returned immediately with an empty hungup_uuids list.
 *
 * Threading: same as Hangup — gRPC thread, no shared state mutation.
 *
 * Cited FACTs: FF-016, FF-022.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/call_cause.h"
#include "osw/control/session_guard.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.hangup_many";

// Attempt to hang up a single UUID. Returns true on success.
// Logs per-uuid errors but does not propagate them — HangupMany
// is best-effort over the full list.
bool HangupOne(const std::string& uuid, switch_call_cause_t cause) noexcept {
    if (uuid.empty()) {
        osw::log::Debug(kSubsystem, "HangupMany: skipping empty uuid");
        return false;
    }

    auto guard = osw::control::SessionGuard::Locate(uuid);
    if (!guard.Valid()) {
        osw::log::Debug(kSubsystem, "HangupMany: uuid=%s not found", uuid.c_str());
        return false;
    }

    switch_channel_t* const ch = guard.Channel();
    if (ch == nullptr) {
        osw::log::Warn(kSubsystem, "HangupMany: channel null for uuid=%s", uuid.c_str());
        return false;
    }

    const auto state_before = osw::raii::fs::ChannelHangup(ch, cause);
    if (state_before >= CS_HANGUP) {
        osw::log::Debug(kSubsystem,
                        "HangupMany: uuid=%s already dead (state=%d)",
                        uuid.c_str(),
                        static_cast<int>(state_before));
        return false;
    }

    return true;
}

}  // namespace

grpc::Status ControlServiceSkeleton::HangupMany(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::HangupManyRequest* req,
    open_switch::control::v1::HangupManyResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    if (req->uuids_size() == 0) {
        return grpc::Status::OK;
    }

    const switch_call_cause_t cause = osw::control::CallCause::FromString(req->cause());

    int hung_count = 0;
    for (const auto& uuid : req->uuids()) {
        if (HangupOne(uuid, cause)) {
            resp->add_hungup_uuids(uuid);
            ++hung_count;
            osw::audit::Emit("osw.control.hangup",
                             {{"uuid", uuid},
                              {"cause", std::string(osw::control::CallCause::ToString(cause))},
                              {"via", "hangup_many"}});
        }
    }

    osw::log::Info(kSubsystem,
                   "HangupMany: %d/%d hung up (cause=%d)",
                   hung_count,
                   req->uuids_size(),
                   static_cast<int>(cause));

    return grpc::Status::OK;
}

}  // namespace osw::control
