/*
 * src/control/handlers/stop_media_stream_handler.cc
 *
 * ControlServiceSkeleton::StopMediaStream — remove the named stream from
 * ActiveMediaStreams, triggering the TearDown sequence defined there.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/stop_media_stream_handler.h"

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

namespace {
constexpr const char* kSubsystem = "control.stop_media_stream";
}  // namespace

grpc::Status HandleStopMediaStream(grpc::ServerContext* /*ctx*/,
                                   const open_switch::control::v1::StopMediaStreamRequest* req,
                                   open_switch::control::v1::StopMediaStreamResponse* resp,
                                   osw::control::ActiveMediaStreams* streams) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (req->stream_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "stream_id required");
    }
    if (!streams) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }

    const bool was_active = streams->Remove(req->stream_id());
    resp->set_was_active(was_active);

    if (was_active) {
        const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
        osw::audit::Emit("control.media.stop",
                         {{"channel_uuid", req->channel_uuid()},
                          {"stream_id", req->stream_id()},
                          {"tenant_id", tenant_id}});
        osw::log::Info(kSubsystem,
                       "StopMediaStream OK: stream_id=%s channel=%s",
                       req->stream_id().c_str(),
                       req->channel_uuid().c_str());
    } else {
        osw::log::Debug(kSubsystem,
                        "StopMediaStream: stream_id=%s not found (idempotent)",
                        req->stream_id().c_str());
    }

    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StopMediaStream(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StopMediaStreamRequest* req,
    open_switch::control::v1::StopMediaStreamResponse* resp) {
    return osw::control::handlers::HandleStopMediaStream(
        ctx, req, resp, active_media_streams_.load(std::memory_order_acquire));
}
