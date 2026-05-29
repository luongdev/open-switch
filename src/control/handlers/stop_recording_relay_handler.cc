/*
 * src/control/handlers/stop_recording_relay_handler.cc
 *
 * W7 Track B StopRecordingRelay RPC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/stop_recording_relay_handler.h"

#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

namespace {
constexpr const char* kSubsystem = "control.stop_recording_relay";
}  // namespace

grpc::Status HandleStopRecordingRelay(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::StopRecordingRelayRequest* req,
    open_switch::control::v1::StopRecordingRelayResponse* resp,
    osw::control::ActiveMediaStreams* streams) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (!streams) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }
    if (req->stream_id().empty() && req->channel_uuid().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "stream_id or channel_uuid required");
    }

    std::uint32_t stopped = 0;
    if (!req->stream_id().empty()) {
        stopped = streams->RemoveIfPurpose(
                      req->stream_id(), open_switch::media::v1::StreamStart::RECORDING_RELAY)
                      ? 1u
                      : 0u;
    } else {
        stopped = static_cast<std::uint32_t>(streams->RemovePurposeForChannel(
            req->channel_uuid(), open_switch::media::v1::StreamStart::RECORDING_RELAY));
    }
    resp->set_streams_stopped(stopped);

    if (stopped > 0) {
        osw::log::Info(kSubsystem,
                       "StopRecordingRelay OK: channel=%s stream_id=%s stopped=%u",
                       req->channel_uuid().c_str(),
                       req->stream_id().c_str(),
                       stopped);
    }

    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StopRecordingRelay(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StopRecordingRelayRequest* req,
    open_switch::control::v1::StopRecordingRelayResponse* resp) {
    return osw::control::handlers::HandleStopRecordingRelay(
        ctx, req, resp, active_media_streams_.load(std::memory_order_acquire));
}
