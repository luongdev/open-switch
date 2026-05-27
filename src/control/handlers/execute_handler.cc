/*
 * src/control/handlers/execute_handler.cc
 *
 * Real implementation of ControlService::Execute (W3 Track B).
 *
 * RPC contract:
 *   Success  → ExecuteResponse {} + audit emit osw.control.execute
 *              (args are secret-redacted before audit).
 *   Failures:
 *     INVALID_ARGUMENT    — empty uuid, empty app, or app not in allow-list.
 *     NOT_FOUND           — SessionGuard::Locate failed.
 *     FAILED_PRECONDITION — channel is hung up before execute started.
 *
 * App allow-list (V1 fixed set):
 *   playback, bridge, transfer, set, hangup, answer, play_and_get_digits.
 *   Any other app name → INVALID_ARGUMENT without calling FS.
 *
 * PII redaction:
 *   Args are sanitized before logging / audit: substrings matching
 *   "<key>=<value>" where key contains "password", "token", or "secret"
 *   (case-insensitive) have the value portion replaced with "[REDACTED]".
 *   The full args string is passed through to FS unchanged.
 *
 * Threading:
 *   V1 is synchronous — the gRPC thread blocks until
 *   switch_core_session_execute_application returns (FF-024).
 *   The SessionGuard read-lock is held for the full duration of the
 *   execute call (FF-016).
 *
 * Cited FACTs: FF-016, FF-024.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/session_guard.h"
#include "osw/control/var_denylist.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.execute";

// V1 fixed allow-list of dialplan applications that may be executed via RPC.
// Removed: bridge, transfer, play_and_get_digits — callers must use the
// dedicated Bridge/BlindTransfer RPCs; play_and_get_digits is deferred to
// a typed V2 RPC. Extend only after security review.
const std::unordered_set<std::string>& AllowedApps() noexcept {
    static const std::unordered_set<std::string> kApps{
        "playback",
        "set",
        "hangup",
        "answer",
    };
    return kApps;
}

[[nodiscard]] bool IsAppAllowed(const std::string& app) noexcept {
    return AllowedApps().count(app) > 0;
}

// Redact values for keys that look like passwords, tokens, or secrets.
// Pattern: <key>=<value> where key contains "password", "token", or "secret"
// (case-insensitive). Value is replaced with "[REDACTED]".
// The regex is compiled once (static local).
[[nodiscard]] std::string RedactArgs(const std::string& args) {
    if (args.empty()) {
        return args;
    }
    // Match <key>=<value> where the key CONTAINS "password", "token", or
    // "secret" as a substring (not just an exact word). This covers compound
    // keys like api_secret_key=... or password_hash=..., not only bare
    // "password=..." or "token=...". Value runs to whitespace or end of string.
    //
    // Pattern: \S*?(?:password|token|secret)\S* — any non-ws chars on either
    // side of the keyword, non-greedy prefix. Still icase via std::regex::icase.
    //
    // NOTE: do NOT prefix the pattern with `(?i)` — GCC's libstdc++ implements
    // ECMAScript regex grammar which does NOT recognise inline mode flags,
    // throwing std::regex_error at construction.
    static const std::regex kRedactPattern(R"((\S*?(?:password|token|secret)\S*)=(\S+))",
                                           std::regex::icase);
    return std::regex_replace(args, kRedactPattern, "$1=[REDACTED]");
}

}  // namespace

grpc::Status ControlServiceSkeleton::Execute(grpc::ServerContext* /*ctx*/,
                                             const open_switch::control::v1::ExecuteRequest* req,
                                             open_switch::control::v1::ExecuteResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    const std::string& uuid = req->uuid();
    const std::string& app = req->app();
    const std::string& args = req->args();

    if (uuid.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "uuid must not be empty");
    }
    if (app.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "app must not be empty");
    }
    if (!IsAppAllowed(app)) {
        osw::log::Debug(
            kSubsystem, "Execute INVALID_ARGUMENT: app='%s' not in allow-list", app.c_str());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "app '" + app + "' is not in the V1 allow-list");
    }

    auto guard = osw::control::SessionGuard::Locate(uuid);
    if (!guard.Valid()) {
        osw::log::Debug(kSubsystem, "Execute NOT_FOUND: uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found: uuid=" + uuid);
    }

    switch_channel_t* const ch = guard.Channel();
    if (ch == nullptr) {
        osw::log::Warn(kSubsystem, "Execute: channel null for uuid=%s", uuid.c_str());
        return grpc::Status(grpc::StatusCode::INTERNAL, "channel pointer null");
    }

    // Pre-execute state check: reject if channel already dead.
    const auto state = osw::raii::fs::ChannelGetState(ch);
    if (state >= CS_HANGUP) {
        osw::log::Debug(kSubsystem,
                        "Execute FAILED_PRECONDITION: channel dead uuid=%s state=%d",
                        uuid.c_str(),
                        static_cast<int>(state));
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "channel already hung up: uuid=" + uuid);
    }

    // When app == "set", the args string is "name=value". Apply the same
    // reserved-var denylist as SetVariables to prevent Execute from being
    // used as a backdoor to set sip_h_*, exec_*, api_on_*, etc.
    if (app == "set" && !args.empty()) {
        const auto eq = args.find('=');
        const std::string var_name = (eq != std::string::npos) ? args.substr(0, eq) : args;
        if (osw::control::IsReservedVar(var_name)) {
            osw::log::Debug(kSubsystem,
                            "Execute INVALID_ARGUMENT: set with reserved var '%s' uuid=%s",
                            var_name.c_str(),
                            uuid.c_str());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "variable name is reserved: '" + var_name + "'");
        }
    }

    // FF-024: execute_application blocks the gRPC thread until the app
    // returns. The session read-lock (via guard) is held for the duration.
    const char* args_cstr = args.empty() ? nullptr : args.c_str();
    const switch_status_t rc =
        osw::raii::fs::ExecuteApplication(guard.get(), app.c_str(), args_cstr);

    if (rc != SWITCH_STATUS_SUCCESS) {
        osw::log::Warn(
            kSubsystem, "Execute FS failure: rc=%d uuid=%s app=%s", rc, uuid.c_str(), app.c_str());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "execute_application failed: rc=" + std::to_string(rc));
    }

    // Audit with redacted args (PII-safe).
    const std::string args_redacted = RedactArgs(args);
    osw::audit::Emit("osw.control.execute",
                     {{"uuid", uuid}, {"app", app}, {"args_redacted", args_redacted}});

    osw::log::Info(kSubsystem, "Execute OK: uuid=%s app=%s", uuid.c_str(), app.c_str());

    return grpc::Status::OK;
}

}  // namespace osw::control
