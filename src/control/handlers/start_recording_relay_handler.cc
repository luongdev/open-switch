/*
 * src/control/handlers/start_recording_relay_handler.cc
 *
 * W7 Track B StartRecordingRelay RPC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/start_recording_relay_handler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/control/active_media_streams.h"
#include "osw/control/session_guard.h"
#include "osw/core/config.h"
#include "osw/events/envelope.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/media/recording_relay.h"
#include "osw/media/stream_client.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

#include "src/control/control_service_skeleton.h"
#include "src/control/handlers/idempotency_utils.h"
#include "src/control/handlers/media_rate.h"

namespace osw::control::handlers {

namespace {

constexpr const char* kSubsystem = "control.start_recording_relay";
constexpr std::uint32_t kReadStreamFlag = static_cast<std::uint32_t>(SMBF_READ_STREAM);
constexpr std::uint32_t kWriteStreamFlag = static_cast<std::uint32_t>(SMBF_WRITE_STREAM);
constexpr std::uint32_t kAssumedPtimeMs = 20;

bool ValidRate(std::uint32_t rate) noexcept {
    return rate == 8000 || rate == 16000 || rate == 24000 || rate == 48000;
}

}  // namespace

grpc::Status HandleStartRecordingRelay(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::StartRecordingRelayRequest* req,
    open_switch::control::v1::StartRecordingRelayResponse* resp,
    osw::media::MediaBugManager* bug_mgr,
    osw::control::ActiveMediaStreams* streams,
    const osw::Config& config) {
    if (!req || !resp) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null request or response");
    }
    if (req->channel_uuid().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "channel_uuid required");
    }
    if (req->relay_endpoint().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "relay_endpoint required");
    }
    if (!bug_mgr || !streams) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media plane not initialised");
    }

    const std::uint32_t rate =
        req->sample_rate_hz() != 0 ? req->sample_rate_hz() : config.recording_default_rate_hz;
    if (!ValidRate(rate)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "sample_rate_hz must be 8000, 16000, 24000, or 48000");
    }

    auto sg = osw::control::SessionGuard::Locate(req->channel_uuid());
    if (!sg.Valid()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
    }
    if (!bug_mgr->HasInjectBug(req->channel_uuid())) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "recording relay requires active TTS or voicebot inject bug");
    }
    grpc::Status write_rate_status =
        ValidateRecordingWriteRate(sg.get(), rate, "StartRecordingRelay write");
    if (!write_rate_status.ok()) {
        return write_rate_status;
    }
    if (req->stereo()) {
        grpc::Status read_rate_status =
            ValidateReadStreamRate(sg.get(), rate, "StartRecordingRelay read");
        if (!read_rate_status.ok()) {
            return read_rate_status;
        }
    }
    sg.Reset();

    const std::string tenant_id = req->has_header() ? req->header().tenant_id() : std::string{};
    const std::string stream_id = osw::events::GenerateUuidV7();

    osw::media::StreamConfig sc;
    sc.stream_id = stream_id;
    sc.channel_uuid = req->channel_uuid();
    sc.tenant_id = tenant_id;
    sc.purpose = open_switch::media::v1::StreamStart::RECORDING_RELAY;
    sc.sample_rate_hz = rate;
    sc.channels = req->stereo() ? 2u : 1u;
    sc.codec = open_switch::media::v1::AudioCodec::PCM_S16LE;
    sc.side = req->stereo() ? open_switch::media::v1::StreamStart::STEREO
                            : open_switch::media::v1::StreamStart::BOTH_MIXED;
    sc.traceparent = req->has_header() ? req->header().traceparent() : std::string{};
    sc.send_ring_capacity_frames = std::max<std::size_t>(
        1, (config.recording_send_ring_ms + kAssumedPtimeMs - 1) / kAssumedPtimeMs);

    osw::media::StreamCallbacks cbs;
    auto channel = grpc::CreateChannel(req->relay_endpoint(), grpc::InsecureChannelCredentials());
    auto client = std::make_unique<osw::media::StreamClient>(
        std::move(channel), std::move(sc), std::move(cbs));

    const grpc::Status open_st = client->Open(5000);
    if (!open_st.ok()) {
        osw::log::Warn(kSubsystem,
                       "StartRecordingRelay: Open failed channel=%s ep=%s: %s",
                       req->channel_uuid().c_str(),
                       req->relay_endpoint().c_str(),
                       open_st.error_message().c_str());
        return open_st;
    }

    sg = osw::control::SessionGuard::Locate(req->channel_uuid());
    if (!sg.Valid()) {
        client->Close();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "channel not found");
    }
    write_rate_status = ValidateRecordingWriteRate(sg.get(), rate, "StartRecordingRelay write");
    if (!write_rate_status.ok()) {
        client->Close();
        return write_rate_status;
    }
    if (req->stereo()) {
        grpc::Status read_rate_status =
            ValidateReadStreamRate(sg.get(), rate, "StartRecordingRelay read");
        if (!read_rate_status.ok()) {
            client->Close();
            return read_rate_status;
        }
    }
    const std::uint32_t fs_read_rate = req->stereo() ? ReadMediaRate(sg.get()).sample_rate_hz : 0u;
    const std::uint32_t fs_write_rate = WriteMediaRate(sg.get()).sample_rate_hz;

    osw::media::MediaBugManager::AttachResult read_attach{};
    if (req->stereo()) {
        osw::media::BugConfig read_cfg;
        read_cfg.purpose = osw::media::Purpose::kRecordingRelay;
        read_cfg.fs_flags = kReadStreamFlag;
        read_cfg.target_rate_hz = rate;
        read_cfg.tenant_id = tenant_id;
        read_cfg.stream_endpoint = req->relay_endpoint();

        read_attach = bug_mgr->Attach(sg.get(), read_cfg);
        if (!read_attach.ok) {
            client->Close();
            return grpc::Status(read_attach.status_code, read_attach.error);
        }
    }

    osw::media::BugConfig write_cfg;
    write_cfg.purpose = osw::media::Purpose::kRecordingRelay;
    write_cfg.fs_flags = kWriteStreamFlag;
    write_cfg.target_rate_hz = rate;
    write_cfg.tenant_id = tenant_id;
    write_cfg.stream_endpoint = req->relay_endpoint();

    auto write_attach = bug_mgr->Attach(sg.get(), write_cfg);
    if (!write_attach.ok) {
        client->Close();
        return grpc::Status(write_attach.status_code, write_attach.error);
    }

    osw::media::RecordingRelayConfig relay_cfg;
    relay_cfg.channel_uuid = req->channel_uuid();
    relay_cfg.tenant_id = tenant_id;
    relay_cfg.stream_id = stream_id;
    relay_cfg.stereo = req->stereo();
    relay_cfg.sample_rate_hz = rate;
    relay_cfg.read_fs_rate_hz = fs_read_rate;
    relay_cfg.write_fs_rate_hz = fs_write_rate;
    relay_cfg.desync_warn_ms = config.stereo_desync_warn_ms;
    relay_cfg.desync_timeout_ms = config.stereo_desync_timeout_ms;

    auto relay = std::make_unique<osw::media::RecordingRelay>(client.get(), std::move(relay_cfg));
    auto* relay_raw = relay.get();

    if (req->stereo()) {
        bug_mgr->SetBugCallback(osw::media::MediaBugManager::BugId(read_attach.handle),
                                reinterpret_cast<void*>(OswRecordingReadTap),
                                relay_raw);
    }
    bug_mgr->SetBugCallback(osw::media::MediaBugManager::BugId(write_attach.handle),
                            reinterpret_cast<void*>(OswRecordingWriteTap),
                            relay_raw);
    relay->Start();

    auto stream = std::make_unique<osw::control::ActiveMediaStream>();
    stream->channel_uuid = req->channel_uuid();
    stream->stream_id = stream_id;
    stream->purpose = open_switch::media::v1::StreamStart::RECORDING_RELAY;
    if (req->stereo()) {
        stream->bugs.push_back(std::move(read_attach.handle));
    }
    stream->bugs.push_back(std::move(write_attach.handle));
    stream->client = std::move(client);
    stream->recording_ctx =
        std::unique_ptr<void, osw::control::RecordingCtxDeleter>(relay.release());

    if (!streams->Insert(std::move(stream))) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "stream_id collision");
    }

    resp->set_stream_id(stream_id);
    resp->set_negotiated_rate_hz(rate);

    (void)osw::audit::EmitSubclass("osw.recording.relay_started",
                                   {{"channel_uuid", req->channel_uuid()},
                                    {"stream_id", stream_id},
                                    {"tenant_id", tenant_id},
                                    {"stereo", req->stereo() ? "true" : "false"}});
    osw::log::Info(kSubsystem,
                   "StartRecordingRelay OK: channel=%s stream_id=%s ep=%s rate=%u stereo=%d",
                   req->channel_uuid().c_str(),
                   stream_id.c_str(),
                   req->relay_endpoint().c_str(),
                   rate,
                   req->stereo() ? 1 : 0);
    return grpc::Status::OK;
}

}  // namespace osw::control::handlers

grpc::Status osw::control::ControlServiceSkeleton::StartRecordingRelay(
    grpc::ServerContext* ctx,
    const open_switch::control::v1::StartRecordingRelayRequest* req,
    open_switch::control::v1::StartRecordingRelayResponse* resp) {
    const osw::Config* config = config_.load(std::memory_order_acquire);
    if (!config) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "media config not initialised");
    }
    const std::string tenant_id = (req && req->has_header()) ? req->header().tenant_id() : "";
    const std::string request_id = (req && req->has_header()) ? req->header().request_id() : "";
    return osw::control::handlers::RunIdempotent(
        idempotency_cache_.load(std::memory_order_acquire),
        "StartRecordingRelay",
        tenant_id,
        request_id,
        resp,
        [&]() {
            return osw::control::handlers::HandleStartRecordingRelay(
                ctx,
                req,
                resp,
                bug_mgr_.load(std::memory_order_acquire),
                active_media_streams_.load(std::memory_order_acquire),
                *config);
        });
}
