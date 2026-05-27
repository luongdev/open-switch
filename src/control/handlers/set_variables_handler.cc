/*
 * src/control/handlers/set_variables_handler.cc
 *
 * Real implementation of ControlService::SetVariables (W3 Track C).
 *
 * RPC contract:
 *   Success  → SetVariablesResponse {} + audit emit osw.control.set_variables
 *              {uuid, var_count}.  Values are NOT emitted (PII risk).
 *   Failures:
 *     INVALID_ARGUMENT   — empty uuid, empty variables map, or any variable
 *                          name containing characters outside [A-Za-z0-9_-].
 *     NOT_FOUND          — SessionGuard::Locate returned invalid.
 *     RESOURCE_EXHAUSTED — more than 64 variables in the request.
 *
 * Variable name validation:
 *   Names are scanned character-by-character (no regex overhead); an invalid
 *   character short-circuits with INVALID_ARGUMENT BEFORE any FS call is
 *   made.  Empty name string is also rejected.
 *
 * All-or-nothing semantics are V2; V1 iterates and calls
 * switch_channel_set_variable on each pair.  If a single call returns
 * non-SUCCESS the handler logs and continues — partial application is
 * acceptable in V1.
 *
 * Threading:
 *   Single gRPC thread per RPC.  SessionGuard held for the full iteration.
 *
 * Cited FACTs: FF-016, FF-026.
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

constexpr const char* kSubsystem = "control.set_variables";
constexpr int kMaxVariables = 64;

// Returns true iff every character of `name` is in [A-Za-z0-9_-].
// Empty names return false.
[[nodiscard]] bool IsValidVarName(const std::string& name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

grpc::Status ControlServiceSkeleton::SetVariables(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::SetVariablesRequest* req,
    open_switch::control::v1::SetVariablesResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    const std::string& uuid = req->uuid();
    if (uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuid must not be empty");
    }

    const auto& vars = req->variables();
    if (vars.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "variables map must not be empty");
    }

    if (static_cast<int>(vars.size()) > kMaxVariables) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "too many variables: max=" + std::to_string(kMaxVariables) +
                                " got=" + std::to_string(vars.size()));
    }

    // Validate all names up-front (before any FS call).
    for (const auto& [name, value] : vars) {
        if (!IsValidVarName(name)) {
            osw::log::Debug(kSubsystem,
                            "SetVariables INVALID_ARGUMENT: bad var name '%s' for uuid=%s",
                            name.c_str(),
                            uuid.c_str());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "invalid variable name: '" + name +
                                    "' (must match [A-Za-z0-9_-]+)");
        }
    }

    auto guard = osw::control::SessionGuard::Locate(uuid);
    if (!guard.Valid()) {
        osw::log::Debug(kSubsystem, "SetVariables NOT_FOUND: uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found: uuid=" + uuid);
    }

    switch_channel_t* const ch = guard.Channel();
    if (ch == nullptr) {
        osw::log::Warn(kSubsystem, "SetVariables: channel null for uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
    }

    // FF-026: FS copies name+value into pool; caller buffers freed after call.
    for (const auto& [name, value] : vars) {
        const switch_status_t rc =
            osw::raii::fs::ChannelSetVariable(ch, name.c_str(), value.c_str());
        if (rc != SWITCH_STATUS_SUCCESS) {
            // V1: log and continue (partial is acceptable).
            osw::log::Warn(kSubsystem,
                           "SetVariables: set_variable failed rc=%d name=%s uuid=%s",
                           rc,
                           name.c_str(),
                           uuid.c_str());
        }
    }

    const int count = static_cast<int>(vars.size());
    // PII: emit count only — NOT individual names or values.
    osw::audit::Emit("osw.control.set_variables",
                     {{"uuid", uuid}, {"var_count", std::to_string(count)}});

    osw::log::Info(kSubsystem,
                   "SetVariables OK: uuid=%s count=%d",
                   uuid.c_str(),
                   count);

    return grpc::Status::OK;
}

}  // namespace osw::control
