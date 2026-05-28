/*
 * src/control/handlers/start_stt_handler.cc
 *
 * ControlServiceSkeleton::StartStt — attach a READ_STREAM media bug +
 * open a StreamClient; bug callback forwards mic frames to the STT service.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_stt_handler.h"

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/control/session_guard.h"
#include "osw/core/config.h"
#include "osw/events/envelope.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/media/stream_client.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"
#include "src/control/handlers/media_bug_callbacks.h"

namespace osw::control::handlers {

namespace {

constexpr const char* kSubsystem = "control.start_stt";
constexpr std::uint32_t kDefaultSampleRateHz = 16000;

constexpr std::uint32_t kReadStreamFlag = static_cast<std::uint32_t>(SMBF_READ_STREAM);

}  // namespace

grpc::Status HandleStartStt(grpc::ServerContext* /*ctx*/,
                            const open_switch::control::v1::StartSttRequest* req,
                            open_switch::control::v1::StartSttResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* streams,
                            const osw::Config& /*config*/) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (req->channel_uuid().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "channel_uuid required");
    }
    if (req->upstream_endpoint().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "upstream_endpoint required");
    }
    const std::uint32_t rate =
        (req->sample_rate_hz() != 0) ? req->sample_rate_hz() : kDefaultSampleRateHz;
    if (rate != 8000 && rate != 16000) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "sample_rate_hz must be 8000 or 16000");
    }
    if (!bug_mgr || !streams) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }

    auto sg = osw::control::SessionGuard::Locate(req->channel_uuid());
    if (!sg.Valid()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
    }

    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};

    osw::media::StreamConfig sc;
    sc.channel_uuid = req->channel_uuid();
    sc.tenant_id = tenant_id;
    sc.purpose = open_switch::media::v1::StreamStart::STT_TRANSCRIBE;
    sc.sample_rate_hz = rate;
    sc.channels = 1;
    sc.codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    for (const auto& [k, v] : req->variables()) {
        sc.variables[k] = v;
    }
    if (!req->language().empty()) {
        sc.variables["language"] = req->language();
    }
    sc.variables["interim_results"] = req->interim_results() ? "true" : "false";
    for (const auto& hint : req->vocabulary_hints()) {
        // Append hints as comma-separated in a single variable.
        if (!sc.variables["vocabulary_hints"].empty()) {
            sc.variables["vocabulary_hints"] += ",";
        }
        sc.variables["vocabulary_hints"] += hint;
    }

    osw::media::StreamCallbacks cbs;
    // on_transcript: emit a Tier-2 osw.stt.transcript audit event.
    cbs.on_transcript = [channel_uuid = req->channel_uuid(),
                         tid = tenant_id](open_switch::media::v1::Transcript t) noexcept {
        osw::audit::Emit("stt.transcript",
                         {{"channel_uuid", channel_uuid},
                          {"tenant_id", tid},
                          {"text", t.text()},
                          {"final", t.final() ? "true" : "false"},
                          {"language", t.language()},
                          {"confidence", std::to_string(t.confidence())}});
    };

    auto channel =
        grpc::CreateChannel(req->upstream_endpoint(), grpc::InsecureChannelCredentials());
    auto client = std::make_unique<osw::media::StreamClient>(
        std::move(channel), std::move(sc), std::move(cbs));

    const grpc::Status open_st = client->Open(5000);
    if (!open_st.ok()) {
        osw::log::Warn(kSubsystem,
                       "StartStt: Open failed channel=%s ep=%s: %s",
                       req->channel_uuid().c_str(),
                       req->upstream_endpoint().c_str(),
                       open_st.error_message().c_str());
        return open_st;
    }

    osw::media::BugConfig bug_cfg;
    bug_cfg.purpose = osw::media::Purpose::kSttTranscribe;
    bug_cfg.fs_flags = kReadStreamFlag;
    bug_cfg.target_rate_hz = rate;
    bug_cfg.tenant_id = tenant_id;

    auto attach = bug_mgr->Attach(sg.get(), bug_cfg);
    if (!attach.ok) {
        client->Close();
        osw::log::Warn(kSubsystem,
                       "StartStt: Attach failed channel=%s: %s",
                       req->channel_uuid().c_str(),
                       attach.error.c_str());
        return grpc::Status(attach.status_code, attach.error);
    }

    // Wire read-tap callback: user_data is the StreamClient*.
    const std::uint64_t bug_id = osw::media::MediaBugManager::BugId(attach.handle);
    bug_mgr->SetBugCallback(bug_id, reinterpret_cast<void*>(OswStreamingReadTap), client.get());

    const std::string stream_id = osw::events::GenerateUuidV7();

    auto stream = std::make_unique<osw::control::ActiveMediaStream>();
    stream->channel_uuid = req->channel_uuid();
    stream->stream_id = stream_id;
    stream->purpose = open_switch::media::v1::StreamStart::STT_TRANSCRIBE;
    stream->bugs.push_back(std::move(attach.handle));
    stream->client = std::move(client);
    // tts_buffer and write_ctx remain null for STT.

    if (!streams->Insert(std::move(stream))) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "stream_id collision");
    }

    resp->set_stream_id(stream_id);
    resp->set_negotiated_codec(open_switch::media::v1::AudioCodec::PCM_S16LE);
    resp->set_negotiated_sample_rate_hz(rate);

    osw::audit::Emit(
        "control.media.start",
        {{"channel_uuid", req->channel_uuid()},
         {"purpose", std::string(osw::media::PurposeName(osw::media::Purpose::kSttTranscribe))},
         {"stream_id", stream_id},
         {"tenant_id", tenant_id}});

    osw::log::Info(kSubsystem,
                   "StartStt OK: channel=%s stream_id=%s ep=%s rate=%u",
                   req->channel_uuid().c_str(),
                   stream_id.c_str(),
                   req->upstream_endpoint().c_str(),
                   rate);
    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StartStt(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartSttRequest* req,
    open_switch::control::v1::StartSttResponse* resp) {
    const osw::Config* config = config_.load(std::memory_order_acquire);
    if (!config) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media config not initialised");
    }
    return osw::control::handlers::HandleStartStt(
        ctx,
        req,
        resp,
        bug_mgr_.load(std::memory_order_acquire),
        active_media_streams_.load(std::memory_order_acquire),
        *config);
}
