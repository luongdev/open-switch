/*
 * include/osw/control/handlers/start_tts_handler.h
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_START_TTS_HANDLER_H_
#define OSW_CONTROL_HANDLERS_START_TTS_HANDLER_H_

// Forward declarations only — keeps this header free of heavy includes.
namespace open_switch::control::v1 {
class StartTtsRequest;
class StartTtsResponse;
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

grpc::Status HandleStartTts(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartTtsRequest* req,
    open_switch::control::v1::StartTtsResponse* resp,
    osw::media::MediaBugManager* bug_mgr,
    osw::control::ActiveMediaStreams* streams,
    const osw::Config& config);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_START_TTS_HANDLER_H_
