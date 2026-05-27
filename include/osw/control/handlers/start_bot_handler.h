/*
 * include/osw/control/handlers/start_bot_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_START_BOT_HANDLER_H_
#define OSW_CONTROL_HANDLERS_START_BOT_HANDLER_H_

namespace open_switch::control::v1 {
class StartBotRequest;
class StartBotResponse;
}  // namespace open_switch::control::v1

namespace grpc {
class ServerContext;
class Status;
}  // namespace grpc

namespace osw {
struct Config;
}  // namespace osw

namespace osw::media {
class MediaBugManager;
}  // namespace osw::media

namespace osw::control {
class ActiveBots;
class ActiveMediaStreams;
}  // namespace osw::control

namespace osw::control::handlers {

grpc::Status HandleStartBot(grpc::ServerContext* ctx,
                            const open_switch::control::v1::StartBotRequest* req,
                            open_switch::control::v1::StartBotResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* streams,
                            osw::control::ActiveBots* active_bots,
                            const osw::Config& config);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_START_BOT_HANDLER_H_
