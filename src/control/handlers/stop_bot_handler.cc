/*
 * src/control/handlers/stop_bot_handler.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/stop_bot_handler.h"

#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

namespace {
constexpr const char* kSubsystem = "control.stop_bot";
}  // namespace

grpc::Status HandleStopBot(grpc::ServerContext* /*ctx*/,
                           const open_switch::control::v1::StopBotRequest* req,
                           open_switch::control::v1::StopBotResponse* resp,
                           osw::control::ActiveBots* active_bots,
                           osw::control::ActiveMediaStreams* streams) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (req->bot_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "bot_id required");
    }
    if (!active_bots || !streams) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }

    const bool was_active = active_bots->Stop(req->bot_id(), streams);
    resp->set_was_active(was_active);

    if (was_active) {
        const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
        osw::audit::EmitSubclass("osw.media.bot.stopped",
                                 {{"bot_id", req->bot_id()}, {"tenant_id", tenant_id}});
        osw::log::Info(kSubsystem, "StopBot OK: bot_id=%s", req->bot_id().c_str());
    } else {
        osw::log::Debug(
            kSubsystem, "StopBot: bot_id=%s not found (idempotent)", req->bot_id().c_str());
    }

    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StopBot(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StopBotRequest* req,
    open_switch::control::v1::StopBotResponse* resp) {
    return osw::control::handlers::HandleStopBot(
        ctx,
        req,
        resp,
        active_bots_.load(std::memory_order_acquire),
        active_media_streams_.load(std::memory_order_acquire));
}
