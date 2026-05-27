/*
 * src/control/handlers/bridge_handler.cc
 *
 * Real implementation of ControlService::Bridge (W3 Track B).
 *
 * RPC contract:
 *   Success  → BridgeResponse {} + audit emit osw.control.bridge.
 *   Failures:
 *     INVALID_ARGUMENT    — either UUID empty, or both UUIDs identical.
 *     NOT_FOUND           — SessionGuard::Locate failed for either UUID.
 *     FAILED_PRECONDITION — either channel is not in CS_ROUTING or
 *                           CS_EXECUTE (i.e. channel state >= CS_HANGUP
 *                           or in setup phase other than those two).
 *
 * Locking discipline (deadlock avoidance — FF-023):
 *   Two concurrent Bridge(A, B) and Bridge(B, A) calls on the same UUID
 *   pair MUST acquire the session guards in a consistent order to prevent
 *   AB-BA deadlock. This handler always acquires the guard for the
 *   lexicographically-lower UUID first, then the higher. Any concurrent
 *   inverse-pair call obeys the same rule and therefore waits on the same
 *   first lock rather than forming a cycle.
 *
 *   The guards are held across the switch_ivr_uuid_bridge call and
 *   released (via dtor) on return. FS's internal locate inside uuid_bridge
 *   acquires the same read-locks; that is safe because FreeSWITCH's
 *   session rwlock is re-entrant from the same thread (it is a read-lock,
 *   not an exclusive lock), so double-acquire does not deadlock.
 *
 * Threading:
 *   Single gRPC thread per RPC. No additional mutex needed — all shared
 *   state is protected by the SessionGuard / FS read-lock discipline.
 *
 * Cited FACTs: FF-016, FF-023.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <algorithm>
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

constexpr const char* kSubsystem = "control.bridge";

// Returns true if the channel state is bridgeable (CS_ROUTING or CS_EXECUTE).
// Any state below CS_ROUTING means the channel hasn't answered yet, and any
// state >= CS_HANGUP means the channel is already tearing down.
[[nodiscard]] bool IsChannelBridgeable(switch_channel_state_t state) noexcept {
    return (state == CS_ROUTING || state == CS_EXECUTE);
}

}  // namespace

grpc::Status ControlServiceSkeleton::Bridge(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::BridgeRequest* req,
    open_switch::control::v1::BridgeResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    const std::string& a_uuid = req->leg_a_uuid();
    const std::string& b_uuid = req->leg_b_uuid();

    if (a_uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "leg_a_uuid must not be empty");
    }
    if (b_uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "leg_b_uuid must not be empty");
    }
    if (a_uuid == b_uuid) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "leg_a_uuid and leg_b_uuid must be different");
    }

    // FF-023: acquire SessionGuards in lexicographic UUID order to prevent
    // AB-BA deadlock with a concurrent Bridge call on the reversed pair.
    const bool a_is_lower = (a_uuid < b_uuid);
    const std::string& first_uuid = a_is_lower ? a_uuid : b_uuid;
    const std::string& second_uuid = a_is_lower ? b_uuid : a_uuid;

    auto first_guard = osw::control::SessionGuard::Locate(first_uuid);
    if (!first_guard.Valid()) {
        osw::log::Debug(kSubsystem, "Bridge NOT_FOUND: uuid=%s", first_uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "session not found: uuid=" + first_uuid);
    }

    auto second_guard = osw::control::SessionGuard::Locate(second_uuid);
    if (!second_guard.Valid()) {
        osw::log::Debug(kSubsystem, "Bridge NOT_FOUND: uuid=%s", second_uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "session not found: uuid=" + second_uuid);
    }

    // Validate that both channels are in a bridgeable state.
    switch_channel_t* const first_ch = first_guard.Channel();
    switch_channel_t* const second_ch = second_guard.Channel();

    if (first_ch == nullptr || second_ch == nullptr) {
        osw::log::Warn(kSubsystem, "Bridge: channel pointer null for one or both UUIDs");
        return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
    }

    const auto first_state = osw::raii::fs::ChannelGetState(first_ch);
    if (!IsChannelBridgeable(first_state)) {
        osw::log::Debug(kSubsystem,
                        "Bridge FAILED_PRECONDITION: uuid=%s state=%d not bridgeable",
                        first_uuid.c_str(),
                        static_cast<int>(first_state));
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "channel not in bridgeable state: uuid=" + first_uuid);
    }

    const auto second_state = osw::raii::fs::ChannelGetState(second_ch);
    if (!IsChannelBridgeable(second_state)) {
        osw::log::Debug(kSubsystem,
                        "Bridge FAILED_PRECONDITION: uuid=%s state=%d not bridgeable",
                        second_uuid.c_str(),
                        static_cast<int>(second_state));
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "channel not in bridgeable state: uuid=" + second_uuid);
    }

    // FF-023: call with originator=a, originatee=b (caller-specified order).
    // Both session guards are still held here.
    const switch_status_t rc = osw::raii::fs::UuidBridge(a_uuid.c_str(), b_uuid.c_str());

    if (rc != SWITCH_STATUS_SUCCESS) {
        osw::log::Warn(kSubsystem,
                       "Bridge FS failure: rc=%d a=%s b=%s",
                       rc,
                       a_uuid.c_str(),
                       b_uuid.c_str());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "switch_ivr_uuid_bridge failed: rc=" + std::to_string(rc));
    }

    // Guards release here (end of scope) — correct per FF-023.

    osw::audit::Emit("osw.control.bridge",
                     {{"a_uuid", a_uuid}, {"b_uuid", b_uuid}});

    osw::log::Info(kSubsystem, "Bridge OK: a=%s b=%s", a_uuid.c_str(), b_uuid.c_str());

    return grpc::Status::OK;
}

}  // namespace osw::control
