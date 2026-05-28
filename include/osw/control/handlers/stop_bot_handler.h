/*
 * include/osw/control/handlers/stop_bot_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_STOP_BOT_HANDLER_H_
#define OSW_CONTROL_HANDLERS_STOP_BOT_HANDLER_H_

namespace open_switch::control::v1 {
class StopBotRequest;
class StopBotResponse;
}  // namespace open_switch::control::v1

namespace grpc {
class ServerContext;
class Status;
}  // namespace grpc

namespace osw::control {
class ActiveBots;
class ActiveMediaStreams;
}  // namespace osw::control

namespace osw::control::handlers {

grpc::Status HandleStopBot(grpc::ServerContext* ctx,
                           const open_switch::control::v1::StopBotRequest* req,
                           open_switch::control::v1::StopBotResponse* resp,
                           osw::control::ActiveBots* active_bots,
                           osw::control::ActiveMediaStreams* streams);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_STOP_BOT_HANDLER_H_
