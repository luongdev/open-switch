/*
 * src/control/handlers/unimplemented.cc
 *
 * Two responsibilities in one TU:
 *   1. `osw::control::handlers::Unimplemented(method, wave)` —
 *      uniform UNIMPLEMENTED grpc::Status builder used by every stub
 *      RPC and reusable from any future handler that wants the same
 *      shape.
 *   2. Method-override bodies for every RPC of ControlServiceSkeleton
 *      EXCEPT Health (real impl lives in health_handler.cc). Each
 *      method body forwards to Unimplemented(...) with the wave that
 *      will land the real implementation.
 *
 * Wave mapping (per W1 contract §"OUT"):
 *   Originate, Hangup, HangupMany, Bridge, Execute, SetVariables,
 *   Hold, Unhold, BlindTransfer  → W3 (Control plane)
 *   SubscribeEvents              → W2 (Event plane)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/unimplemented.h"

#include <sstream>
#include <string>

#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

grpc::Status Unimplemented(std::string_view method, std::string_view wave) {
    std::ostringstream oss;
    oss << "not yet implemented (wave " << wave << " coming)";
    const std::string msg = oss.str();
    osw::log::Debug("control",
                    "%.*s: UNIMPLEMENTED (wave %.*s)",
                    static_cast<int>(method.size()),
                    method.data(),
                    static_cast<int>(wave.size()),
                    wave.data());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, msg);
}

}  // namespace osw::control::handlers

namespace osw::control {

// --- W3-bound: control RPCs ----------------------------------------------

grpc::Status ControlServiceSkeleton::Originate(grpc::ServerContext*,
                                               const open_switch::control::v1::OriginateRequest*,
                                               open_switch::control::v1::OriginateResponse*) {
    return handlers::Unimplemented("Originate", "W3");
}

grpc::Status ControlServiceSkeleton::Hangup(grpc::ServerContext*,
                                            const open_switch::control::v1::HangupRequest*,
                                            open_switch::control::v1::HangupResponse*) {
    return handlers::Unimplemented("Hangup", "W3");
}

grpc::Status ControlServiceSkeleton::HangupMany(grpc::ServerContext*,
                                                const open_switch::control::v1::HangupManyRequest*,
                                                open_switch::control::v1::HangupManyResponse*) {
    return handlers::Unimplemented("HangupMany", "W3");
}

grpc::Status ControlServiceSkeleton::Bridge(grpc::ServerContext*,
                                            const open_switch::control::v1::BridgeRequest*,
                                            open_switch::control::v1::BridgeResponse*) {
    return handlers::Unimplemented("Bridge", "W3");
}

grpc::Status ControlServiceSkeleton::Execute(grpc::ServerContext*,
                                             const open_switch::control::v1::ExecuteRequest*,
                                             open_switch::control::v1::ExecuteResponse*) {
    return handlers::Unimplemented("Execute", "W3");
}

grpc::Status ControlServiceSkeleton::SetVariables(
    grpc::ServerContext*,
    const open_switch::control::v1::SetVariablesRequest*,
    open_switch::control::v1::SetVariablesResponse*) {
    return handlers::Unimplemented("SetVariables", "W3");
}

grpc::Status ControlServiceSkeleton::Hold(grpc::ServerContext*,
                                          const open_switch::control::v1::HoldRequest*,
                                          open_switch::control::v1::HoldResponse*) {
    return handlers::Unimplemented("Hold", "W3");
}

grpc::Status ControlServiceSkeleton::Unhold(grpc::ServerContext*,
                                            const open_switch::control::v1::UnholdRequest*,
                                            open_switch::control::v1::UnholdResponse*) {
    return handlers::Unimplemented("Unhold", "W3");
}

grpc::Status ControlServiceSkeleton::BlindTransfer(
    grpc::ServerContext*,
    const open_switch::control::v1::BlindTransferRequest*,
    open_switch::control::v1::BlindTransferResponse*) {
    return handlers::Unimplemented("BlindTransfer", "W3");
}

// --- W2-bound: events streaming -----------------------------------------

grpc::Status ControlServiceSkeleton::SubscribeEvents(
    grpc::ServerContext*,
    const open_switch::control::v1::SubscribeEventsRequest*,
    grpc::ServerWriter<open_switch::events::v1::EventEnvelope>*) {
    return handlers::Unimplemented("SubscribeEvents", "W2");
}

}  // namespace osw::control
