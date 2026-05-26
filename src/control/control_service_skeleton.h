/*
 * src/control/control_service_skeleton.h
 *
 * ControlServiceSkeleton — concrete grpc service that overrides every
 * method of open_switch::control::v1::ControlService::Service. W1
 * ships a working Health handler; all other RPCs return a uniform
 * UNIMPLEMENTED status via osw::control::handlers::Unimplemented.
 *
 * Private to src/control/. Header lives here (not under include/)
 * because the skeleton consumes the generated proto types and we do
 * NOT want a public dependency on the gRPC C++ API surface from
 * outside the control subsystem.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_
#define OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_

#include <atomic>
#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.grpc.pb.h"

namespace osw {
class Health;

namespace control {

class ControlServiceSkeleton final
    : public open_switch::control::v1::ControlService::Service {
 public:
    /// `health` is the module-wide Health aggregator. Non-owning.
    explicit ControlServiceSkeleton(Health* health) noexcept;

    /// Set version strings reported by Health RPC. Called by GrpcServer
    /// before the server starts serving.
    void SetVersions(std::string module_version,
                     std::string freeswitch_version);

    // --- Health (real impl) ----------------------------------------
    grpc::Status Health(grpc::ServerContext*                                ctx,
                        const open_switch::control::v1::HealthRequest*     req,
                        open_switch::control::v1::HealthResponse*          resp) override;

    // --- Unimplemented RPCs ----------------------------------------
    grpc::Status Originate(grpc::ServerContext*                                ctx,
                           const open_switch::control::v1::OriginateRequest*  req,
                           open_switch::control::v1::OriginateResponse*       resp) override;

    grpc::Status Hangup(grpc::ServerContext*                              ctx,
                        const open_switch::control::v1::HangupRequest*    req,
                        open_switch::control::v1::HangupResponse*         resp) override;

    grpc::Status HangupMany(grpc::ServerContext*                              ctx,
                            const open_switch::control::v1::HangupManyRequest* req,
                            open_switch::control::v1::HangupManyResponse*      resp) override;

    grpc::Status Bridge(grpc::ServerContext*                              ctx,
                        const open_switch::control::v1::BridgeRequest*    req,
                        open_switch::control::v1::BridgeResponse*         resp) override;

    grpc::Status Execute(grpc::ServerContext*                              ctx,
                         const open_switch::control::v1::ExecuteRequest*   req,
                         open_switch::control::v1::ExecuteResponse*        resp) override;

    grpc::Status SetVariables(grpc::ServerContext*                             ctx,
                              const open_switch::control::v1::SetVariablesRequest* req,
                              open_switch::control::v1::SetVariablesResponse*      resp) override;

    grpc::Status Hold(grpc::ServerContext*                              ctx,
                      const open_switch::control::v1::HoldRequest*      req,
                      open_switch::control::v1::HoldResponse*           resp) override;

    grpc::Status Unhold(grpc::ServerContext*                              ctx,
                        const open_switch::control::v1::UnholdRequest*    req,
                        open_switch::control::v1::UnholdResponse*         resp) override;

    grpc::Status BlindTransfer(grpc::ServerContext*                                ctx,
                               const open_switch::control::v1::BlindTransferRequest* req,
                               open_switch::control::v1::BlindTransferResponse*      resp) override;

    grpc::Status SubscribeEvents(
        grpc::ServerContext*                                               ctx,
        const open_switch::control::v1::SubscribeEventsRequest*            req,
        grpc::ServerWriter<open_switch::events::v1::EventEnvelope>*        writer) override;

 private:
    osw::Health* health_;
    std::string  module_version_;
    std::string  freeswitch_version_;
};

}  // namespace control
}  // namespace osw

#endif  // OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_
