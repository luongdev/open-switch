/*
 * src/control/handlers/hangup_handler.cc
 *
 * Real implementation of ControlService::Hangup (W3 Track A).
 *
 * RPC contract:
 *   Success  → HangupResponse {} + audit emit osw.control.hangup.
 *   Failures:
 *     INVALID_ARGUMENT    — empty uuid.
 *     NOT_FOUND           — SessionGuard::Locate returned invalid
 *                           (UUID unknown or session tearing down).
 *     FAILED_PRECONDITION — channel already dead (state >= CS_HANGUP).
 *
 * Threading:
 *   Single gRPC thread per RPC. SessionGuard is acquired, channel_hangup
 *   called under the read-lock, guard released. No mutex needed.
 *
 * Memory: no allocation beyond the SessionGuard RAII stack object.
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

constexpr const char* kSubsystem = "control.hangup";

}  // namespace

grpc::Status ControlServiceSkeleton::Hangup(grpc::ServerContext* /*ctx*/,
                                            const open_switch::control::v1::HangupRequest* req,
                                            open_switch::control::v1::HangupResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    const std::string& uuid = req->uuid();
    if (uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuid must not be empty");
    }

    auto guard = osw::control::SessionGuard::Locate(uuid);
    if (!guard.Valid()) {
        osw::log::Debug(kSubsystem, "Hangup NOT_FOUND: uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found: uuid=" + uuid);
    }

    switch_channel_t* const ch = guard.Channel();
    if (ch == nullptr) {
        // Session located but channel pointer null — internal FS inconsistency.
        osw::log::Warn(kSubsystem, "Hangup: channel null for uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
    }

    // Resolve the cause code (empty / missing → NORMAL_CLEARING).
    const switch_call_cause_t cause = osw::control::CallCause::FromString(req->cause());

    // FF-022: pre-read state BEFORE calling hangup. If the channel is
    // already >= CS_HANGUP we must not call hangup (it would be a no-op
    // and we can't distinguish "just killed" from "already dead" from the
    // return value of switch_channel_perform_hangup — the returned state
    // is the NEW state after the transition, not the state before; on a
    // live channel the returned state is CS_HANGUP, which is
    // indistinguishable from an already-dead channel).
    const auto pre_state = osw::raii::fs::ChannelGetState(ch);
    if (pre_state >= CS_HANGUP) {
        osw::log::Debug(
            kSubsystem, "Hangup FAILED_PRECONDITION: channel already dead uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "channel already hung up: uuid=" + uuid);
    }

    osw::raii::fs::ChannelHangup(ch, cause);

    osw::audit::Emit(
        "osw.control.hangup",
        {{"uuid", uuid}, {"cause", std::string(osw::control::CallCause::ToString(cause))}});

    osw::log::Info(
        kSubsystem, "Hangup OK: uuid=%s cause=%d", uuid.c_str(), static_cast<int>(cause));

    return grpc::Status::OK;
}

}  // namespace osw::control
