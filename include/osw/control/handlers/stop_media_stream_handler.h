/*
 * include/osw/control/handlers/stop_media_stream_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_STOP_MEDIA_STREAM_HANDLER_H_
#define OSW_CONTROL_HANDLERS_STOP_MEDIA_STREAM_HANDLER_H_

namespace open_switch::control::v1 {
class StopMediaStreamRequest;
class StopMediaStreamResponse;
}  // namespace open_switch::control::v1

namespace grpc {
class ServerContext;
class Status;
}  // namespace grpc

namespace osw::control {
class ActiveMediaStreams;
}  // namespace osw::control

namespace osw::control::handlers {

grpc::Status HandleStopMediaStream(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StopMediaStreamRequest* req,
    open_switch::control::v1::StopMediaStreamResponse* resp,
    osw::control::ActiveMediaStreams* streams);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_STOP_MEDIA_STREAM_HANDLER_H_
