/*
 * src/control/handlers/start_bot_handler.cc
 *
 * W7 Track D StartBot. The first real slice supports TTS_BROADCAST with one
 * upstream stream and one WRITE_REPLACE bug/fanout queue per target.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_bot_handler.h"

#include <set>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"

#include "osw/control/active_bots.h"
#include "osw/core/config.h"
#include "osw/events/envelope.h"
#include "osw/media/bot_session.h"
#include "osw/media/bug_manager.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"
#include "src/control/handlers/media_bug_callbacks.h"

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
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "duplicate target channel UUID");
        }
    }

    if (!req.write_target_channel_uuids().empty() && req.purpose() != StartBotRequest::WHISPER) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "write_target_channel_uuids is only valid for WHISPER");
    }
    if (req.purpose() == StartBotRequest::WHISPER) {
        if (req.write_target_channel_uuids().empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "WHISPER requires write_target_channel_uuids");
        }
        for (const auto& uuid : req.write_target_channel_uuids()) {
            if (!seen.contains(uuid)) {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                    "write target not in target_channel_uuids");
            }
        }
    }

    switch (req.purpose()) {
        case StartBotRequest::TTS_BROADCAST:
        case StartBotRequest::STT_LISTEN:
        case StartBotRequest::VOICEBOT_DUPLEX:
        case StartBotRequest::WHISPER:
            break;
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

}  // namespace

grpc::Status HandleStartBot(grpc::ServerContext* ctx,
                            const StartBotRequest* req,
                            open_switch::control::v1::StartBotResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* /*streams*/,
                            osw::control::ActiveBots* active_bots,
                            const osw::Config& config) {
    (void)ctx;
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
    const std::string request_id = req->has_header() ? req->header().request_id() : std::string{};
    const std::string traceparent = req->has_header() ? req->header().traceparent() : std::string{};
    osw::log::Info(kSubsystem,
                   "event=osw.start_bot.received request_id=%s tenant_id=%s traceparent=%s "
                   "purpose=%s target_count=%d write_target_count=%d sample_rate_hz=%u",
                   request_id.c_str(),
                   tenant_id.c_str(),
                   traceparent.c_str(),
                   PurposeName(req->purpose()),
                   req->target_channel_uuids_size(),
                   req->write_target_channel_uuids_size(),
                   req->sample_rate_hz());

    if (!bug_mgr || !active_bots) {
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

    const std::string bot_id = osw::events::GenerateUuidV7();

    osw::media::BotSessionConfig bot_cfg;
    bot_cfg.bot_id = bot_id;
    bot_cfg.tenant_id = tenant_id;
    bot_cfg.upstream_endpoint = req->upstream_endpoint();
    bot_cfg.traceparent = traceparent;
    bot_cfg.purpose = req->purpose();
    bot_cfg.sample_rate_hz = rate;
    bot_cfg.start_message = req->start_message();
    bot_cfg.target_queue_ms = config.bot_target_queue_ms;
    bot_cfg.drain_timeout_ms =
        req->drain_timeout_ms() != 0 ? req->drain_timeout_ms() : config.bot_drain_timeout_ms;
    for (const auto& channel_uuid : req->target_channel_uuids()) {
        bot_cfg.target_channel_uuids.push_back(channel_uuid);
    }
    for (const auto& channel_uuid : req->write_target_channel_uuids()) {
        bot_cfg.write_target_channel_uuids.push_back(channel_uuid);
    }
    for (const auto& [key, value] : req->variables()) {
        bot_cfg.variables[key] = value;
    }

    auto channel =
        grpc::CreateChannel(req->upstream_endpoint(), grpc::InsecureChannelCredentials());
    auto session = std::make_unique<osw::media::BotSession>(std::move(bot_cfg), std::move(channel));

    grpc::Status open_st = session->Open(5000);
    if (!open_st.ok()) {
        osw::log::Warn(kSubsystem,
                       "StartBot: upstream open failed bot_id=%s ep=%s: %s",
                       bot_id.c_str(),
                       req->upstream_endpoint().c_str(),
                       open_st.error_message().c_str());
        return open_st;
    }

    grpc::Status attach_st = session->Attach(*bug_mgr,
                                             reinterpret_cast<void*>(OswBotWriteReplace),
                                             reinterpret_cast<void*>(OswBotReadTap));
    if (!attach_st.ok()) {
        osw::audit::EmitSubclass(
            "osw.media.bot.target_attach_failed",
            {{"bot_id", bot_id}, {"tenant_id", tenant_id}, {"error", attach_st.error_message()}});
        osw::log::Warn(kSubsystem,
                       "StartBot: attach failed bot_id=%s: %s",
                       bot_id.c_str(),
                       attach_st.error_message().c_str());
        session->Stop();
        return attach_st;
    }

    ActiveBot bot;
    bot.bot_id = bot_id;
    bot.target_channel_uuids = session->TargetUuids();
    bot.session = std::move(session);

    const ActiveBotInsertResult insert_result =
        active_bots->Insert(std::move(bot), config.max_bots_per_channel);
    if (insert_result != ActiveBotInsertResult::kInserted) {
        if (insert_result == ActiveBotInsertResult::kChannelCapacityExceeded) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "channel already has maximum active bots");
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, "bot_id collision");
    }

    resp->set_bot_id(bot_id);
    resp->set_negotiated_rate_hz(rate);

    osw::audit::EmitSubclass(
        "osw.media.bot.started",
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
