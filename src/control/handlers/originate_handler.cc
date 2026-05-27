/*
 * src/control/handlers/originate_handler.cc
 *
 * Real implementation of ControlService::Originate (W3 Track A).
 *
 * RPC contract:
 *   Success  → OriginateResponse { channel_uuid } + audit emit
 *              osw.control.originate.
 *   Failures:
 *     INVALID_ARGUMENT     — no endpoints, empty endpoint, timeout ≤ 0.
 *     FAILED_PRECONDITION  — switch_ivr_originate returned non-success
 *                            (e.g. USER_BUSY, NO_ANSWER).
 *     DEADLINE_EXCEEDED    — originate timed out.
 *     UNAVAILABLE          — FS returned null bleg (FS not ready).
 *
 * Threading:
 *   This handler blocks the gRPC thread for the full originate duration
 *   (V1 synchronous). The gRPC runtime provides a dedicated thread per
 *   RPC; no shared state is mutated. No mutex needed.
 *
 * Memory:
 *   - OriginateOptions owns the ovars event; it is transferred to
 *     switch_ivr_originate via ReleaseOvars(), which consumes it.
 *   - If originate succeeds, the bleg session read-lock is acquired and
 *     held while we extract the UUID, then immediately released via
 *     SessionRwunlock — we do NOT hold the bleg lock for the rest of
 *     the RPC (the uuid string is all we need).
 *
 * Cited FACTs: FF-021.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/call_cause.h"
#include "osw/control/originate_options.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.originate";

// Map a switch_call_cause_t to the appropriate gRPC status code when
// originate returns a non-success FS status.
[[nodiscard]] grpc::Status MapOriginateFailure(switch_call_cause_t cause) noexcept {
    // SWITCH_CAUSE_ORIGINATOR_CANCEL (487) is the classic "timed out
    // waiting for answer" cause (allotted_timeout also maps here).
    // SWITCH_CAUSE_ALLOTTED_TIMEOUT (802) is the FS internal timeout.
    if (cause == SWITCH_CAUSE_ORIGINATOR_CANCEL || cause == SWITCH_CAUSE_ALLOTTED_TIMEOUT ||
        cause == SWITCH_CAUSE_PROGRESS_TIMEOUT || cause == SWITCH_CAUSE_MEDIA_TIMEOUT) {
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                            "originate timed out before answer");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        std::string("originate failed: cause=") +
                            std::string(osw::control::CallCause::ToString(cause)));
}

}  // namespace

grpc::Status ControlServiceSkeleton::Originate(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::OriginateRequest* req,
    open_switch::control::v1::OriginateResponse* resp) {
    if (req == nullptr || resp == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }

    // Validate and materialise the request into originate parameters.
    auto opts = osw::control::OriginateOptions::Build(*req);
    if (!opts.Valid()) {
        osw::log::Debug(kSubsystem, "Originate INVALID_ARGUMENT: %s", opts.ErrorMessage().c_str());
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, opts.ErrorMessage());
    }

    switch_core_session_t* bleg = nullptr;
    switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;

    // Transfer ownership of ovars to switch_ivr_originate (FF-021:
    // FS consumes the event regardless of outcome).
    switch_event_t* ovars = opts.ReleaseOvars();

    const switch_status_t rc =
        osw::raii::fs::OriginateSession(nullptr,
                                        &bleg,
                                        &cause,
                                        opts.dial_string().c_str(),
                                        opts.timeout_sec(),
                                        opts.cid_name().empty() ? nullptr : opts.cid_name().c_str(),
                                        opts.cid_num().empty() ? nullptr : opts.cid_num().c_str(),
                                        ovars);

    if (rc != SWITCH_STATUS_SUCCESS) {
        osw::log::Warn(
            kSubsystem, "Originate FS failure: rc=%d cause=%d", rc, static_cast<int>(cause));
        return MapOriginateFailure(cause);
    }

    // FF-021: on success, *bleg is the new session (we own the rwlock).
    if (bleg == nullptr) {
        // FS returned SUCCESS but null bleg — treat as UNAVAILABLE.
        osw::log::Warn(kSubsystem, "Originate: FS returned SUCCESS but bleg is null");
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "FS returned null session");
    }

    // Extract the UUID while we hold the read-lock, then release
    // immediately (FF-021: caller must rwunlock once done).
    const char* uuid_cstr = osw::raii::fs::SessionGetUuid(bleg);
    const std::string uuid = (uuid_cstr != nullptr) ? std::string(uuid_cstr) : "";
    osw::raii::fs::SessionRwunlock(bleg);

    if (uuid.empty()) {
        osw::log::Warn(kSubsystem, "Originate: bleg UUID is empty after originate");
        return grpc::Status(grpc::StatusCode::INTERNAL, "originated session has no UUID");
    }

    resp->set_channel_uuid(uuid);

    osw::audit::Emit("osw.control.originate",
                     {{"uuid", uuid},
                      {"dest", opts.dial_string()},
                      {"cid_name", opts.cid_name()},
                      {"cid_num", opts.cid_num()}});

    osw::log::Info(
        kSubsystem, "Originate OK: uuid=%s dest=%s", uuid.c_str(), opts.dial_string().c_str());

    return grpc::Status::OK;
}

}  // namespace osw::control
