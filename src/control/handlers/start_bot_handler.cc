/*
 * src/control/handlers/start_bot_handler.cc
 *
 * W7 Track D StartBot facade. This first implementation reuses the W6
 * single-target media handlers per target, so the demo path can call one
 * logical StartBot and still get module-owned bug lifecycle + W6.6 silence
 * driving on parked channels.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_bot_handler.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/control/handlers/start_tts_handler.h"
#include "osw/control/handlers/start_voicebot_handler.h"
#include "osw/core/config.h"
#include "osw/events/envelope.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

namespace {

constexpr const char* kSubsystem = "control.start_bot";
constexpr std::uint32_t kDefaultSampleRateHz = 16000;

using open_switch::control::v1::StartBotRequest;

const char* PurposeName(StartBotRequest::Purpose purpose) noexcept {
    switch (purpose) {
    case StartBotRequest::TTS_BROADCAST:
        return "tts_broadcast";
    case StartBotRequest::STT_LISTEN:
        return "stt_listen";
    case StartBotRequest::VOICEBOT_DUPLEX:
        return "voicebot_duplex";
    case StartBotRequest::WHISPER:
        return "whisper";
    default:
        return "unspecified";
    }
}

grpc::Status ValidateStartBot(const StartBotRequest& req,
                              const osw::Config& config,
                              std::uint32_t* rate_out) {
    if (req.target_channel_uuids().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "target_channel_uuids required");
    }
    if (static_cast<std::uint32_t>(req.target_channel_uuids().size()) > config.bot_max_targets) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "too many target_channel_uuids");
    }
    if (req.upstream_endpoint().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "upstream_endpoint required");
    }

    std::set<std::string> seen;
    for (const auto& uuid : req.target_channel_uuids()) {
        if (uuid.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "target channel UUID is empty");
        }
        if (!seen.insert(uuid).second) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "duplicate target channel UUID");
        }
    }

    if (!req.write_target_channel_uuids().empty() && req.purpose() != StartBotRequest::WHISPER) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "write_target_channel_uuids is only valid for WHISPER");
    }
    if (req.drain_timeout_ms() != 0) {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            "per-bot drain_timeout_ms override is not supported yet");
    }

    switch (req.purpose()) {
    case StartBotRequest::TTS_BROADCAST:
    case StartBotRequest::VOICEBOT_DUPLEX:
        break;
    case StartBotRequest::STT_LISTEN:
    case StartBotRequest::WHISPER:
        return grpc::Status(
            grpc::StatusCode::UNIMPLEMENTED,
            "StartBot STT_LISTEN/WHISPER requires W7 fanout/read path; TTS_BROADCAST and "
            "VOICEBOT_DUPLEX are available");
    default:
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "purpose required");
    }

    const std::uint32_t rate =
        (req.sample_rate_hz() != 0) ? req.sample_rate_hz() : kDefaultSampleRateHz;
    if (rate != 8000 && rate != 16000) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "sample_rate_hz must be 8000 or 16000");
    }
    *rate_out = rate;
    return grpc::Status::OK;
}

void CopyCommonTtsFields(const StartBotRequest& req,
                         std::string_view channel_uuid,
                         std::uint32_t rate,
                         open_switch::control::v1::StartTtsRequest* out) {
    out->mutable_header()->CopyFrom(req.header());
    out->set_channel_uuid(std::string(channel_uuid));
    out->set_upstream_endpoint(req.upstream_endpoint());
    out->set_sample_rate_hz(rate);
    out->set_start_message(req.start_message());
    for (const auto& [key, value] : req.variables()) {
        (*out->mutable_variables())[key] = value;
    }
    out->mutable_buffer_override()->CopyFrom(req.buffer_override());
}

void CopyCommonVoicebotFields(const StartBotRequest& req,
                              std::string_view channel_uuid,
                              std::uint32_t rate,
                              open_switch::control::v1::StartVoicebotRequest* out) {
    out->mutable_header()->CopyFrom(req.header());
    out->set_channel_uuid(std::string(channel_uuid));
    out->set_upstream_endpoint(req.upstream_endpoint());
    out->set_sample_rate_hz(rate);
    out->set_start_message(req.start_message());
    for (const auto& [key, value] : req.variables()) {
        (*out->mutable_variables())[key] = value;
    }
    out->mutable_buffer_override()->CopyFrom(req.buffer_override());
}

void RollBackStreams(const std::vector<std::string>& stream_ids, ActiveMediaStreams* streams) {
    if (!streams) {
        return;
    }
    for (const auto& stream_id : stream_ids) {
        streams->Remove(stream_id);
    }
}

}  // namespace

grpc::Status HandleStartBot(grpc::ServerContext* ctx,
                            const StartBotRequest* req,
                            open_switch::control::v1::StartBotResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* streams,
                            osw::control::ActiveBots* active_bots,
                            const osw::Config& config) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (!bug_mgr || !streams || !active_bots) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }

    std::uint32_t rate = 0;
    grpc::Status valid = ValidateStartBot(*req, config, &rate);
    if (!valid.ok()) {
        return valid;
    }

    for (const auto& channel_uuid : req->target_channel_uuids()) {
        if (active_bots->ChannelAtCapacity(channel_uuid, config.max_bots_per_channel)) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel already has maximum active bots");
        }
    }

    std::vector<std::string> stream_ids;
    stream_ids.reserve(req->target_channel_uuids().size());

    for (const auto& channel_uuid : req->target_channel_uuids()) {
        if (req->purpose() == StartBotRequest::TTS_BROADCAST) {
            open_switch::control::v1::StartTtsRequest child_req;
            open_switch::control::v1::StartTtsResponse child_resp;
            CopyCommonTtsFields(*req, channel_uuid, rate, &child_req);
            grpc::Status st =
                HandleStartTts(ctx, &child_req, &child_resp, bug_mgr, streams, config);
            if (!st.ok()) {
                RollBackStreams(stream_ids, streams);
                return st;
            }
            stream_ids.push_back(child_resp.stream_id());
            continue;
        }

        open_switch::control::v1::StartVoicebotRequest child_req;
        open_switch::control::v1::StartVoicebotResponse child_resp;
        CopyCommonVoicebotFields(*req, channel_uuid, rate, &child_req);
        grpc::Status st =
            HandleStartVoicebot(ctx, &child_req, &child_resp, bug_mgr, streams, config);
        if (!st.ok()) {
            RollBackStreams(stream_ids, streams);
            return st;
        }
        stream_ids.push_back(child_resp.stream_id());
    }

    ActiveBot bot;
    const std::string bot_id = osw::events::GenerateUuidV7();
    bot.bot_id = bot_id;
    for (const auto& channel_uuid : req->target_channel_uuids()) {
        bot.target_channel_uuids.push_back(channel_uuid);
    }
    bot.stream_ids = stream_ids;

    const ActiveBotInsertResult insert_result =
        active_bots->Insert(std::move(bot), config.max_bots_per_channel);
    if (insert_result != ActiveBotInsertResult::kInserted) {
        RollBackStreams(stream_ids, streams);
        if (insert_result == ActiveBotInsertResult::kChannelCapacityExceeded) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel already has maximum active bots");
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, "bot_id collision");
    }

    resp->set_bot_id(bot_id);
    resp->set_negotiated_rate_hz(rate);

    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
    osw::audit::Emit("osw.media.bot.started",
                     {{"bot_id", bot_id},
                      {"tenant_id", tenant_id},
                      {"purpose", PurposeName(req->purpose())},
                      {"target_count", std::to_string(req->target_channel_uuids().size())}});
    osw::log::Info(kSubsystem,
                   "StartBot OK: bot_id=%s purpose=%s targets=%d rate=%u",
                   bot_id.c_str(),
                   PurposeName(req->purpose()),
                   req->target_channel_uuids_size(),
                   rate);
    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StartBot(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartBotRequest* req,
    open_switch::control::v1::StartBotResponse* resp) {
    const osw::Config* config = config_.load(std::memory_order_acquire);
    if (!config) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media config not initialised");
    }
    return osw::control::handlers::HandleStartBot(
        ctx,
        req,
        resp,
        bug_mgr_.load(std::memory_order_acquire),
        active_media_streams_.load(std::memory_order_acquire),
        active_bots_.load(std::memory_order_acquire),
        *config);
}
