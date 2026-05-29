/*
 * include/osw/control/handlers/stop_recording_relay_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_STOP_RECORDING_RELAY_HANDLER_H_
#define OSW_CONTROL_HANDLERS_STOP_RECORDING_RELAY_HANDLER_H_

namespace open_switch::control::v1 {
class StopRecordingRelayRequest;
class StopRecordingRelayResponse;
}  // namespace open_switch::control::v1

namespace grpc {
class ServerContext;
class Status;
}  // namespace grpc

namespace osw::control {
class ActiveMediaStreams;
}  // namespace osw::control

namespace osw::control::handlers {

grpc::Status HandleStopRecordingRelay(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StopRecordingRelayRequest* req,
    open_switch::control::v1::StopRecordingRelayResponse* resp,
    osw::control::ActiveMediaStreams* streams);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_STOP_RECORDING_RELAY_HANDLER_H_
