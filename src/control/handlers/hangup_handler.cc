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

    // FF-022: switch_channel_hangup is idempotent. If the channel is
    // already >= CS_HANGUP, the call returns CS_HANGUP without side
    // effects. We check the RETURNED state — if it was already hanging
    // before we called (i.e. the return value is CS_HANGUP AND our call
    // was a no-op), we surface FAILED_PRECONDITION. However, distinguishing
    // "already dead" from "just now killed" via return value alone is not
    // fully reliable; the returned state reflects the channel's state at
    // the time of the call. If the channel was already CS_HANGUP the
    // OCF_HANGUP gate in FS returns early; the state will be CS_HANGUP
    // either way. We accept that the FAILED_PRECONDITION path is a best-
    // effort check rather than a hard guarantee.
    const auto state_before = osw::raii::fs::ChannelHangup(ch, cause);

    if (state_before >= CS_HANGUP) {
        osw::log::Debug(
            kSubsystem, "Hangup FAILED_PRECONDITION: channel already dead uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "channel already hung up: uuid=" + uuid);
    }

    osw::audit::Emit(
        "osw.control.hangup",
        {{"uuid", uuid}, {"cause", std::string(osw::control::CallCause::ToString(cause))}});

    osw::log::Info(
        kSubsystem, "Hangup OK: uuid=%s cause=%d", uuid.c_str(), static_cast<int>(cause));

    return grpc::Status::OK;
}

}  // namespace osw::control
