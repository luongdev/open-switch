/*
 * tests/support/media_bridge_harness.h
 *
 * In-process MediaBridge test server for media/control integration tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef TESTS_SUPPORT_MEDIA_BRIDGE_HARNESS_H_
#define TESTS_SUPPORT_MEDIA_BRIDGE_HARNESS_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "open_switch/media/v1/media.grpc.pb.h"
#include "open_switch/media/v1/media.pb.h"

namespace osw::test {

class RecordingMediaBridgeService final : public open_switch::media::v1::MediaBridge::Service {
  public:
    grpc::Status Stream(
        grpc::ServerContext*,
        grpc::ServerReaderWriter<open_switch::media::v1::FromService,
                                 open_switch::media::v1::FromModule>* stream) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stream_count_;
        }

        open_switch::media::v1::FromModule req;
        if (!stream->Read(&req) || !req.has_start()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected StreamStart");
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            starts_.push_back(req.start());
        }

        open_switch::media::v1::FromService ready_msg;
        auto* ready = ready_msg.mutable_ready();
        ready->set_sample_rate_hz(req.start().sample_rate_hz());
        ready->set_channels(req.start().channels());
        ready->set_codec(req.start().codec());
        ready->set_server_stream_id("media-bridge-harness");
        if (!stream->Write(ready_msg)) {
            return grpc::Status::OK;
        }

        for (const auto& frame : ServiceAudioSnapshot()) {
            open_switch::media::v1::FromService audio_msg;
            *audio_msg.mutable_audio() = frame;
            if (!stream->Write(audio_msg)) {
                return grpc::Status::OK;
            }
        }

        open_switch::media::v1::FromModule msg;
        while (stream->Read(&msg)) {
            if (msg.has_audio()) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    received_audio_.push_back(msg.audio());
                }
                cv_.notify_all();
            }
        }
        cv_.notify_all();
        return grpc::Status::OK;
    }

    void SetServiceAudio(std::vector<open_switch::media::v1::AudioFrame> frames) {
        std::lock_guard<std::mutex> lock(mu_);
        service_audio_ = std::move(frames);
    }

    bool WaitForReceivedAudioCount(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, timeout, [&] { return received_audio_.size() >= count; });
    }

    std::vector<open_switch::media::v1::AudioFrame> ReceivedAudioSnapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return received_audio_;
    }

    std::vector<open_switch::media::v1::StreamStart> StartsSnapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return starts_;
    }

    int StreamCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return stream_count_;
    }

  private:
    std::vector<open_switch::media::v1::AudioFrame> ServiceAudioSnapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return service_audio_;
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    int stream_count_ = 0;
    std::vector<open_switch::media::v1::StreamStart> starts_;
    std::vector<open_switch::media::v1::AudioFrame> received_audio_;
    std::vector<open_switch::media::v1::AudioFrame> service_audio_;
};

class MediaBridgeHarness {
  public:
    bool Start() {
        service_ = std::make_unique<RecordingMediaBridgeService>();
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port_);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        return server_ != nullptr && bound_port_ > 0;
    }

    void Stop() {
        if (server_) {
            server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
            server_->Wait();
            server_.reset();
        }
        service_.reset();
        bound_port_ = 0;
    }

    std::shared_ptr<grpc::Channel> MakeChannel() const {
        return grpc::CreateChannel("127.0.0.1:" + std::to_string(bound_port_),
                                   grpc::InsecureChannelCredentials());
    }

    RecordingMediaBridgeService& service() { return *service_; }
    const RecordingMediaBridgeService& service() const { return *service_; }

  private:
    std::unique_ptr<RecordingMediaBridgeService> service_;
    std::unique_ptr<grpc::Server> server_;
    int bound_port_ = 0;
};

inline open_switch::media::v1::AudioFrame MakeAudioProto(std::uint64_t seq,
                                                         std::uint32_t duration_samples,
                                                         std::uint32_t channels,
                                                         std::int16_t sample_value) {
    open_switch::media::v1::AudioFrame frame;
    frame.set_seq(seq);
    frame.set_timestamp_samples(seq * duration_samples);
    frame.set_duration_samples(duration_samples);
    std::vector<std::int16_t> samples(static_cast<std::size_t>(duration_samples) * channels,
                                      sample_value);
    frame.set_payload(reinterpret_cast<const char*>(samples.data()),
                      samples.size() * sizeof(std::int16_t));
    return frame;
}

inline std::vector<std::int16_t> PayloadToSamples(const open_switch::media::v1::AudioFrame& frame) {
    std::vector<std::int16_t> samples(frame.payload().size() / sizeof(std::int16_t));
    std::memcpy(samples.data(), frame.payload().data(), frame.payload().size());
    return samples;
}

}  // namespace osw::test

#endif  // TESTS_SUPPORT_MEDIA_BRIDGE_HARNESS_H_
