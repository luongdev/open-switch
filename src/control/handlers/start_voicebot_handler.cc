/*
 * src/control/handlers/start_voicebot_handler.cc
 *
 * ControlServiceSkeleton::StartVoicebot — attach TWO media bugs under one
 * stream_id:
 *   - kVoicebotDuplexRead  (MID_READ, SMBF_READ_STREAM):  mic → StreamClient
 *   - kVoicebotDuplexWrite (INJECT,   SMBF_WRITE_REPLACE): TtsPlayoutBuffer → speaker
 *
 * The StreamClient carries both directions over a single bidi gRPC stream.
 * on_audio pushes TTS frames into the TtsPlayoutBuffer (write side).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_voicebot_handler.h"

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
#include "osw/security/eavesdrop_marker.h"
#include "osw/security/eavesdrop_policy.h"

#include "src/control/control_service_skeleton.h"
#include "src/control/handlers/idempotency_utils.h"
#include "src/control/handlers/media_bug_callbacks.h"
#include "src/control/handlers/media_rate.h"

namespace osw::control::handlers {

namespace {

constexpr const char* kSubsystem = "control.start_voicebot";
constexpr std::uint32_t kDefaultSampleRateHz = 16000;

constexpr std::uint32_t kReadStreamFlag = static_cast<std::uint32_t>(SMBF_READ_STREAM);
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
                        "StartVoicebot: buffer_override.jitter_buffer_ms=%u clamped to %u",
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
                        "StartVoicebot: buffer_override.preroll_ms=%u clamped to %u",
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

void MaybeWarnRecordBeforeInject(switch_core_session_t* session,
                                 const std::string& channel_uuid,
                                 const std::string& tenant_id,
                                 const osw::Config& config) noexcept {
    if (!config.warn_record_before_inject || !session) {
        return;
    }
    const std::uint32_t native_record_count =
        osw::raii::fs::MediaBugCount(session, "record_session");
    if (native_record_count == 0) {
        return;
    }
    osw::audit::EmitSubclass(
        "osw.recording.warn_record_before_inject",
        {{"channel_uuid", channel_uuid},
         {"tenant_id", tenant_id},
         {"inject_purpose", "voicebot"},
         {"native_record_count", std::to_string(native_record_count)},
         {"remediation",
          "Reorder dialplan: attach bot media before record_session, or use StartRecordingRelay"}});
    osw::log::Warn(kSubsystem,
                   "StartVoicebot: record_session bug already present before inject channel=%s "
                   "tenant_id=%s count=%u",
                   channel_uuid.c_str(),
                   tenant_id.c_str(),
                   native_record_count);
}

}  // namespace

grpc::Status HandleStartVoicebot(grpc::ServerContext* /*ctx*/,
                                 const open_switch::control::v1::StartVoicebotRequest* req,
                                 open_switch::control::v1::StartVoicebotResponse* resp,
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

    {
        auto sg = osw::control::SessionGuard::Locate(req->channel_uuid());
        if (!sg.Valid()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
        }
        grpc::Status read_rate = ValidateReadStreamRate(sg.get(), rate, "StartVoicebot read");
        if (!read_rate.ok()) {
            return read_rate;
        }
        grpc::Status write_rate = ValidateWriteStreamRate(sg.get(), rate, "StartVoicebot write");
        if (!write_rate.ok()) {
            return write_rate;
        }
    }
    const std::size_t replaced = streams->RemoveWriteReplaceForChannel(req->channel_uuid());
    if (replaced > 0) {
        osw::log::Info(kSubsystem,
                       "StartVoicebot: removed %zu prior write-replace stream(s) on channel=%s",
                       replaced,
                       req->channel_uuid().c_str());
    }

    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
    const std::string stream_id = osw::events::GenerateUuidV7();

    // -----------------------------------------------------------------------
    // Build TtsPlayoutBuffer for the write (bot→caller) side.
    // -----------------------------------------------------------------------
    const auto& ov = req->buffer_override();
    const std::uint32_t jitter_ms = ResolveBufferMs(ov, config);
    const std::uint32_t preroll_ms = ResolvePrerollMs(ov, config, jitter_ms);

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

    // -----------------------------------------------------------------------
    // Build StreamClient — bidi stream carries mic audio up and TTS audio
    // down. on_audio callback pushes TTS frames into the jitter buffer.
    // -----------------------------------------------------------------------
    osw::media::StreamConfig sc;
    sc.stream_id = stream_id;
    sc.channel_uuid = req->channel_uuid();
    sc.tenant_id = tenant_id;
    sc.purpose = open_switch::media::v1::StreamStart::VOICEBOT_DUPLEX;
    sc.sample_rate_hz = rate;
    sc.channels = 1;
    sc.codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    sc.traceparent = req->has_header() ? req->header().traceparent() : std::string{};
    sc.start_message = req->start_message();
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
                       "StartVoicebot: Open failed channel=%s ep=%s: %s",
                       req->channel_uuid().c_str(),
                       req->upstream_endpoint().c_str(),
                       open_st.error_message().c_str());
        return open_st;
    }

    auto sg = osw::control::SessionGuard::Locate(req->channel_uuid());
    if (!sg.Valid()) {
        client->Close();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
    }
    grpc::Status read_rate = ValidateReadStreamRate(sg.get(), rate, "StartVoicebot read");
    if (!read_rate.ok()) {
        client->Close();
        return read_rate;
    }
    grpc::Status write_rate = ValidateWriteStreamRate(sg.get(), rate, "StartVoicebot write");
    if (!write_rate.ok()) {
        client->Close();
        return write_rate;
    }
    const std::uint32_t fs_read_rate = ReadMediaRate(sg.get()).sample_rate_hz;
    const std::uint32_t fs_write_rate = WriteMediaRate(sg.get()).sample_rate_hz;

    // -----------------------------------------------------------------------
    // Bug 1: read tap (MID_READ, SMBF_READ_STREAM).
    // -----------------------------------------------------------------------
    osw::media::BugConfig read_cfg;
    read_cfg.purpose = osw::media::Purpose::kVoicebotDuplexRead;
    read_cfg.fs_flags = kReadStreamFlag;
    read_cfg.target_rate_hz = rate;
    read_cfg.tenant_id = tenant_id;

    auto read_attach = bug_mgr->Attach(sg.get(), read_cfg);
    if (!read_attach.ok) {
        client->Close();
        osw::log::Warn(kSubsystem,
                       "StartVoicebot: Attach(read) failed channel=%s: %s",
                       req->channel_uuid().c_str(),
                       read_attach.error.c_str());
        return grpc::Status(read_attach.status_code, read_attach.error);
    }

    const std::uint64_t read_bug_id = osw::media::MediaBugManager::BugId(read_attach.handle);
    auto read_ctx = std::make_unique<osw::control::handlers::ReadCallbackCtx>();
    read_ctx->client = client.get();
    read_ctx->stream_rate_hz = rate;
    read_ctx->fs_rate_hz = fs_read_rate;
    bug_mgr->SetBugCallback(
        read_bug_id, reinterpret_cast<void*>(OswStreamingReadTap), read_ctx.get());

    // -----------------------------------------------------------------------
    // Bug 2: write replace (INJECT, SMBF_WRITE_REPLACE).
    // -----------------------------------------------------------------------
    osw::media::BugConfig write_cfg;
    write_cfg.purpose = osw::media::Purpose::kVoicebotDuplexWrite;
    write_cfg.fs_flags = kWriteReplaceFlag;
    write_cfg.target_rate_hz = rate;
    write_cfg.tenant_id = tenant_id;

    auto write_attach = bug_mgr->Attach(sg.get(), write_cfg);
    if (!write_attach.ok) {
        // read bug was attached — BugHandle dtor will Detach it when
        // read_attach goes out of scope after we return.
        client->Close();
        osw::log::Warn(kSubsystem,
                       "StartVoicebot: Attach(write) failed channel=%s: %s",
                       req->channel_uuid().c_str(),
                       write_attach.error.c_str());
        return grpc::Status(write_attach.status_code, write_attach.error);
    }
    MaybeWarnRecordBeforeInject(sg.get(), req->channel_uuid(), tenant_id, config);
    osw::security::MarkBotSession(
        sg.get(), "voicebot", osw::security::ResolveEffectivePolicy(config, tenant_id), tenant_id);

    auto write_ctx = std::make_unique<osw::control::handlers::WriteCallbackCtx>();
    write_ctx->client = client.get();
    write_ctx->buffer = buffer.get();
    write_ctx->stream_id = stream_id;
    write_ctx->stream_rate_hz = rate;
    write_ctx->fs_rate_hz = fs_write_rate;

    const std::uint64_t write_bug_id = osw::media::MediaBugManager::BugId(write_attach.handle);
    bug_mgr->SetBugCallback(
        write_bug_id, reinterpret_cast<void*>(OswStreamingWriteReplace), write_ctx.get());

    // -----------------------------------------------------------------------
    // Register.
    // -----------------------------------------------------------------------
    buffer->SetTenantId(tenant_id);

    auto stream = std::make_unique<osw::control::ActiveMediaStream>();
    stream->channel_uuid = req->channel_uuid();
    stream->stream_id = stream_id;
    stream->purpose = open_switch::media::v1::StreamStart::VOICEBOT_DUPLEX;
    // Read bug first, write bug second — TearDown calls bugs.clear() which
    // destructs in reverse order (write detached before read), which is safe.
    stream->bugs.push_back(std::move(read_attach.handle));
    stream->bugs.push_back(std::move(write_attach.handle));
    stream->client = std::move(client);
    stream->tts_buffer = std::move(buffer);
    stream->write_ctx = std::unique_ptr<void, osw::control::WriteCtxDeleter>(write_ctx.release());
    stream->read_ctx = std::unique_ptr<void, osw::control::ReadCtxDeleter>(read_ctx.release());

    if (!streams->Insert(std::move(stream))) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "stream_id collision");
    }

    resp->set_stream_id(stream_id);
    resp->set_negotiated_codec(open_switch::media::v1::AudioCodec::PCM_S16LE);
    resp->set_negotiated_sample_rate_hz(rate);

    osw::audit::Emit(
        "control.media.start",
        {{"channel_uuid", req->channel_uuid()},
         {"purpose",
          std::string(osw::media::PurposeName(osw::media::Purpose::kVoicebotDuplexRead))},
         {"stream_id", stream_id},
         {"tenant_id", tenant_id}});

    osw::log::Info(kSubsystem,
                   "StartVoicebot OK: channel=%s stream_id=%s ep=%s rate=%u",
                   req->channel_uuid().c_str(),
                   stream_id.c_str(),
                   req->upstream_endpoint().c_str(),
                   rate);
    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StartVoicebot(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartVoicebotRequest* req,
    open_switch::control::v1::StartVoicebotResponse* resp) {
    const osw::Config* config = config_.load(std::memory_order_acquire);
    if (!config) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media config not initialised");
    }
    const std::string tenant_id = (req && req->has_header()) ? req->header().tenant_id() : "";
    const std::string request_id = (req && req->has_header()) ? req->header().request_id() : "";
    return osw::control::handlers::RunIdempotent(
        idempotency_cache_.load(std::memory_order_acquire),
        "StartVoicebot",
        tenant_id,
        request_id,
        resp,
        [&]() {
            return osw::control::handlers::HandleStartVoicebot(
                ctx,
                req,
                resp,
                bug_mgr_.load(std::memory_order_acquire),
                active_media_streams_.load(std::memory_order_acquire),
                *config);
        });
}
