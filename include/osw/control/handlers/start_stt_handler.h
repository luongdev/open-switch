/*
 * include/osw/control/handlers/start_stt_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_START_STT_HANDLER_H_
#define OSW_CONTROL_HANDLERS_START_STT_HANDLER_H_

namespace open_switch::control::v1 {
class StartSttRequest;
class StartSttResponse;
}  // namespace open_switch::control::v1

namespace grpc {
class ServerContext;
class Status;
}  // namespace grpc

namespace osw::media {
class MediaBugManager;
}  // namespace osw::media

namespace osw::control {
class ActiveMediaStreams;
}  // namespace osw::control

namespace osw {
struct Config;
}  // namespace osw

namespace osw::control::handlers {

grpc::Status HandleStartStt(grpc::ServerContext* ctx,
                            const open_switch::control::v1::StartSttRequest* req,
                            open_switch::control::v1::StartSttResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* streams,
                            const osw::Config& config);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_START_STT_HANDLER_H_
