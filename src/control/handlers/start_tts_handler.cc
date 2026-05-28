/*
 * src/control/handlers/start_tts_handler.cc
 *
 * ControlServiceSkeleton::StartTts — attach a WRITE_REPLACE media bug +
 * open a StreamClient to the upstream TTS service + wire the TtsPlayoutBuffer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_tts_handler.h"

#include <algorithm>
#include <chrono>
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
#include "osw/media/tts_playout_buffer.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"
#include "src/control/handlers/media_bug_callbacks.h"

namespace osw::control::handlers {

namespace {

constexpr const char* kSubsystem = "control.start_tts";
constexpr std::uint32_t kDefaultSampleRateHz = 16000;

constexpr std::uint32_t kWriteReplaceFlag = static_cast<std::uint32_t>(SMBF_WRITE_REPLACE);

std::uint32_t ResolveBufferMs(const open_switch::control::v1::TtsBufferOverride& ov,
                              const osw::Config& cfg) noexcept {
    if (ov.jitter_buffer_ms() == 0) {
        return cfg.tts_jitter_buffer_ms;
    }
    constexpr std::uint32_t kMin = 200;
    const std::uint32_t clamped =
        std::clamp(ov.jitter_buffer_ms(), kMin, cfg.tts_max_jitter_buffer_ms);
    if (clamped != ov.jitter_buffer_ms()) {
        osw::log::Debug(kSubsystem,
                        "StartTts: buffer_override.jitter_buffer_ms=%u clamped to %u",
                        ov.jitter_buffer_ms(),
                        clamped);
    }
    return clamped;
}

std::uint32_t ResolvePrerollMs(const open_switch::control::v1::TtsBufferOverride& ov,
                               const osw::Config& cfg,
                               std::uint32_t jitter_ms) noexcept {
    if (ov.preroll_ms() == 0) {
        return std::min(cfg.tts_preroll_ms, jitter_ms);
    }
    constexpr std::uint32_t kMin = 50;
    const std::uint32_t clamped = std::clamp(ov.preroll_ms(), kMin, jitter_ms);
    if (clamped != ov.preroll_ms()) {
        osw::log::Debug(kSubsystem,
                        "StartTts: buffer_override.preroll_ms=%u clamped to %u",
                        ov.preroll_ms(),
                        clamped);
    }
    return clamped;
}

osw::media::TtsPlayoutBuffer::UnderrunPolicy ParseUnderrunPolicy(const std::string& s) noexcept {
    if (s == "repeat_last") {
        return osw::media::TtsPlayoutBuffer::UnderrunPolicy::kRepeatLast;
    }
    return osw::media::TtsPlayoutBuffer::UnderrunPolicy::kSilence;
}

}  // namespace

grpc::Status HandleStartTts(grpc::ServerContext* /*ctx*/,
                            const open_switch::control::v1::StartTtsRequest* req,
                            open_switch::control::v1::StartTtsResponse* resp,
                            osw::media::MediaBugManager* bug_mgr,
                            osw::control::ActiveMediaStreams* streams,
                            const osw::Config& config) {
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

    // Locate session.
    auto sg = osw::control::SessionGuard::Locate(req->channel_uuid());
    if (!sg.Valid()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
    }
    const std::size_t replaced = streams->RemoveWriteReplaceForChannel(req->channel_uuid());
    if (replaced > 0) {
        osw::log::Info(kSubsystem,
                       "StartTts: removed %zu prior write-replace stream(s) on channel=%s",
                       replaced,
                       req->channel_uuid().c_str());
    }

    // Build TtsPlayoutBuffer.
    const auto& ov = req->buffer_override();
    const std::uint32_t jitter_ms = ResolveBufferMs(ov, config);
    const std::uint32_t preroll_ms = ResolvePrerollMs(ov, config, jitter_ms);
    const std::string stream_id = osw::events::GenerateUuidV7();

    osw::media::TtsPlayoutBuffer::Config buf_cfg;
    buf_cfg.target_ms = std::chrono::milliseconds(jitter_ms);
    buf_cfg.preroll_ms = std::chrono::milliseconds(preroll_ms);
    buf_cfg.high_water_ms =
        std::chrono::milliseconds(std::max(config.tts_high_water_ms, jitter_ms));
    buf_cfg.underrun = ParseUnderrunPolicy(config.tts_underrun_policy);
    buf_cfg.channel_sample_rate_hz = rate;
    buf_cfg.channels = 1;

    auto buffer = std::make_unique<osw::media::TtsPlayoutBuffer>(buf_cfg);
    auto* buf_raw = buffer.get();
    buffer->SetStreamId(stream_id);

    // Build StreamClient. on_audio callback pushes into the jitter buffer.
    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};

    osw::media::StreamConfig sc;
    sc.channel_uuid = req->channel_uuid();
    sc.tenant_id = tenant_id;
    sc.purpose = open_switch::media::v1::StreamStart::TTS_PLAYBACK;
    sc.sample_rate_hz = rate;
    sc.channels = 1;
    sc.codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    sc.start_message = req->start_message();
    sc.half_close_writes_after_start = true;
    for (const auto& [k, v] : req->variables()) {
        sc.variables[k] = v;
    }

    osw::media::StreamCallbacks cbs;
    cbs.on_audio = [buf_raw](osw::media::AudioFrame f) noexcept { buf_raw->Push(std::move(f)); };
    cbs.on_done = [buf_raw](grpc::Status /*status*/) noexcept {
        if (buf_raw) {
            buf_raw->SignalEndOfStream();
        }
    };

    auto channel =
        grpc::CreateChannel(req->upstream_endpoint(), grpc::InsecureChannelCredentials());
    auto client = std::make_unique<osw::media::StreamClient>(
        std::move(channel), std::move(sc), std::move(cbs));

    const grpc::Status open_st = client->Open(5000);
    if (!open_st.ok()) {
        osw::log::Warn(kSubsystem,
                       "StartTts: Open failed channel=%s ep=%s: %s",
                       req->channel_uuid().c_str(),
                       req->upstream_endpoint().c_str(),
                       open_st.error_message().c_str());
        return open_st;
    }

    // Attach WRITE_REPLACE media bug.
    osw::media::BugConfig bug_cfg;
    bug_cfg.purpose = osw::media::Purpose::kTtsPlayback;
    bug_cfg.fs_flags = kWriteReplaceFlag;
    bug_cfg.target_rate_hz = rate;
    bug_cfg.tenant_id = tenant_id;

    auto attach = bug_mgr->Attach(sg.get(), bug_cfg);
    if (!attach.ok) {
        client->Close();
        osw::log::Warn(kSubsystem,
                       "StartTts: Attach failed channel=%s: %s",
                       req->channel_uuid().c_str(),
                       attach.error.c_str());
        return grpc::Status(attach.status_code, attach.error);
    }

    // Allocate WriteCallbackCtx and wire it into the BugCallbackContext
    // via MediaBugManager::SetBugCallback.
    auto write_ctx = std::make_unique<osw::control::handlers::WriteCallbackCtx>();
    write_ctx->client = client.get();
    write_ctx->buffer = buffer.get();

    const std::uint64_t bug_id = osw::media::MediaBugManager::BugId(attach.handle);
    bug_mgr->SetBugCallback(
        bug_id, reinterpret_cast<void*>(OswStreamingWriteReplace), write_ctx.get());

    // Register.
    buffer->SetTenantId(tenant_id);

    auto stream = std::make_unique<osw::control::ActiveMediaStream>();
    stream->channel_uuid = req->channel_uuid();
    stream->stream_id = stream_id;
    stream->purpose = open_switch::media::v1::StreamStart::TTS_PLAYBACK;
    stream->bugs.push_back(std::move(attach.handle));
    stream->client = std::move(client);
    stream->tts_buffer = std::move(buffer);
    // Transfer ownership of write_ctx into the stream so it is freed
    // after the bug callback can no longer fire (TearDown ordering).
    stream->write_ctx = std::unique_ptr<void, osw::control::WriteCtxDeleter>(write_ctx.release());

    if (!streams->Insert(std::move(stream))) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "stream_id collision");
    }

    resp->set_stream_id(stream_id);
    resp->set_negotiated_codec(open_switch::media::v1::AudioCodec::PCM_S16LE);
    resp->set_negotiated_sample_rate_hz(rate);

    osw::audit::Emit(
        "control.media.start",
        {{"channel_uuid", req->channel_uuid()},
         {"purpose", std::string(osw::media::PurposeName(osw::media::Purpose::kTtsPlayback))},
         {"stream_id", stream_id},
         {"tenant_id", tenant_id}});

    osw::log::Info(kSubsystem,
                   "StartTts OK: channel=%s stream_id=%s ep=%s rate=%u",
                   req->channel_uuid().c_str(),
                   stream_id.c_str(),
                   req->upstream_endpoint().c_str(),
                   rate);
    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

// ---------------------------------------------------------------------------
// ControlServiceSkeleton method override
// ---------------------------------------------------------------------------

grpc::Status osw::control::ControlServiceSkeleton::StartTts(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartTtsRequest* req,
    open_switch::control::v1::StartTtsResponse* resp) {
    const osw::Config* config = config_.load(std::memory_order_acquire);
    if (!config) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media config not initialised");
    }
    return osw::control::handlers::HandleStartTts(
        ctx,
        req,
        resp,
        bug_mgr_.load(std::memory_order_acquire),
        active_media_streams_.load(std::memory_order_acquire),
        *config);
}
