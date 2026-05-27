/*
 * src/control/handlers/blind_transfer_handler.cc
 *
 * Real implementation of ControlService::BlindTransfer (W3 Track B).
 *
 * RPC contract:
 *   Success  → BlindTransferResponse {} + audit emit
 *              osw.control.blind_transfer.
 *   Failures:
 *     INVALID_ARGUMENT — empty uuid or empty destination (extension).
 *     NOT_FOUND        — SessionGuard::Locate failed.
 *
 * NULL passthrough (FF-025):
 *   `dialplan` and `context` are optional proto fields. An empty string
 *   is passed to FS as nullptr (causing FS to use its own defaults: "XML"
 *   for dialplan, the channel's current context for context). A non-empty
 *   string is passed verbatim.
 *
 * Threading:
 *   Single gRPC thread per RPC. switch_ivr_session_transfer queues the
 *   transfer on the channel's state machine and returns promptly; the
 *   gRPC thread is NOT blocked for the duration of the transfer. The
 *   SessionGuard read-lock (FF-016) is held for the duration of the call
 *   and released via the guard's dtor on return.
 *
 * Cited FACTs: FF-016, FF-025.
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

constexpr const char* kSubsystem = "control.blind_transfer";

// FF-025: empty optional proto string → nullptr (FS defaults kick in).
[[nodiscard]] const char* OptionalStringToPtr(const std::string& s) noexcept {
    return s.empty() ? nullptr : s.c_str();
}

}  // namespace

grpc::Status ControlServiceSkeleton::BlindTransfer(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::BlindTransferRequest* req,
    open_switch::control::v1::BlindTransferResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    const std::string& uuid = req->uuid();
    const std::string& destination = req->destination();
    const std::string& dialplan = req->dialplan();
    const std::string& context = req->context();

    if (uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuid must not be empty");
    }
    if (destination.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "destination (extension) must not be empty");
    }

    auto guard = osw::control::SessionGuard::Locate(uuid);
    if (!guard.Valid()) {
        osw::log::Debug(kSubsystem, "BlindTransfer NOT_FOUND: uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found: uuid=" + uuid);
    }

    // FF-025: pass nullptr for empty dialplan/context so FS uses defaults.
    const char* dialplan_ptr = OptionalStringToPtr(dialplan);
    const char* context_ptr = OptionalStringToPtr(context);

    const switch_status_t rc =
        osw::raii::fs::SessionTransfer(guard.get(), destination.c_str(), dialplan_ptr, context_ptr);

    if (rc != SWITCH_STATUS_SUCCESS) {
        osw::log::Warn(kSubsystem,
                       "BlindTransfer FS failure: rc=%d uuid=%s dest=%s",
                       rc,
                       uuid.c_str(),
                       destination.c_str());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "session_transfer failed: rc=" + std::to_string(rc));
    }

    osw::audit::Emit("osw.control.blind_transfer",
                     {{"uuid", uuid},
                      {"extension", destination},
                      {"dialplan", dialplan.empty() ? "(default)" : dialplan},
                      {"context", context.empty() ? "(default)" : context}});

    osw::log::Info(kSubsystem,
                   "BlindTransfer OK: uuid=%s dest=%s dialplan=%s context=%s",
                   uuid.c_str(),
                   destination.c_str(),
                   dialplan.empty() ? "(null)" : dialplan.c_str(),
                   context.empty() ? "(null)" : context.c_str());

    return grpc::Status::OK;
}

}  // namespace osw::control
