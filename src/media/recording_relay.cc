/*
 * src/media/recording_relay.cc
 *
 * W7 Track B recording relay lifecycle + FS media bug callbacks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/recording_relay.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/audio_frame.h"
#include "osw/observability/audit.h"
#include "osw/observability/log.h"

namespace {

constexpr const char* kSubsystem = "media.recording_relay";
constexpr std::uint32_t kBothInterleavedChannel = 3;
constexpr std::chrono::milliseconds kTickInterval{5};
constexpr std::chrono::minutes kAuditRateLimit{1};

#if defined(OSW_TEST_FS_MOCK)
constexpr int kAbcTypeRead = 1;
constexpr int kAbcTypeWrite = 2;
constexpr int kAbcTypeInit = 0;
constexpr int kAbcTypeClose = 8;
#else
constexpr int kAbcTypeRead = static_cast<int>(SWITCH_ABC_TYPE_READ);
constexpr int kAbcTypeWrite = static_cast<int>(SWITCH_ABC_TYPE_WRITE);
constexpr int kAbcTypeInit = static_cast<int>(SWITCH_ABC_TYPE_INIT);
constexpr int kAbcTypeClose = static_cast<int>(SWITCH_ABC_TYPE_CLOSE);
#endif

std::int16_t ReadInt16Le(const std::vector<std::uint8_t>& bytes, std::size_t sample_index) {
    const std::size_t offset = sample_index * sizeof(std::int16_t);
    const auto value =
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
    return static_cast<std::int16_t>(value);
}

std::int16_t AverageSamples(std::int16_t left, std::int16_t right) noexcept {
    const int mixed = (static_cast<int>(left) + static_cast<int>(right)) / 2;
    return static_cast<std::int16_t>(
        std::clamp(mixed,
                   static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<int>(std::numeric_limits<std::int16_t>::max())));
}

#if !defined(OSW_TEST_FS_MOCK)
std::vector<std::int16_t> MonoSamplesFromFrame(const switch_frame_t& frame) {
    const auto* src = static_cast<const std::int16_t*>(frame.data);
    const std::size_t total = frame.datalen / sizeof(std::int16_t);
    const std::uint32_t channels =
        frame.channels > 0 ? static_cast<std::uint32_t>(frame.channels) : 1u;
    if (channels <= 1u) {
        return std::vector<std::int16_t>(src, src + total);
    }

    std::vector<std::int16_t> mono;
    mono.reserve(total / channels);
    for (std::size_t i = 0; i < total; i += channels) {
        mono.push_back(src[i]);
    }
    return mono;
}
#endif

}  // namespace

namespace osw::media {

RecordingRelay::RecordingRelay(StreamClient* client, RecordingRelayConfig config)
    : client_(client),
      config_(std::move(config)),
      pairer_(StereoFramePairer::Config{
          config_.sample_rate_hz, config_.desync_warn_ms, config_.desync_timeout_ms}) {}

RecordingRelay::~RecordingRelay() noexcept {
    Stop();
}

void RecordingRelay::Start() noexcept {
    if (tick_thread_.joinable()) {
        return;
    }
    stopped_.store(false, std::memory_order_release);
    try {
        tick_thread_ = std::thread(&RecordingRelay::TickLoop, this);
    } catch (...) {
        stopped_.store(true, std::memory_order_release);
    }
}

void RecordingRelay::Stop() noexcept {
    stopped_.store(true, std::memory_order_release);
    if (tick_thread_.joinable()) {
        tick_thread_.join();
    }
}

void RecordingRelay::EmitStopped() noexcept {
    bool expected = false;
    if (!stopped_audit_emitted_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    (void)osw::audit::EmitSubclass("osw.recording.relay_stopped",
                                   {{"channel_uuid", config_.channel_uuid},
                                    {"stream_id", config_.stream_id},
                                    {"tenant_id", config_.tenant_id}});
}

bool RecordingRelay::Stopped() const noexcept {
    return stopped_.load(std::memory_order_acquire);
}

void RecordingRelay::PushReadFrame(std::uint64_t fs_timestamp_samples,
                                   std::span<const std::int16_t> samples) noexcept {
    PushSide(true, fs_timestamp_samples, samples);
}

void RecordingRelay::PushWriteFrame(std::uint64_t fs_timestamp_samples,
                                    std::span<const std::int16_t> samples) noexcept {
    PushSide(false, fs_timestamp_samples, samples);
}

void RecordingRelay::PushSide(bool left,
                              std::uint64_t fs_timestamp_samples,
                              std::span<const std::int16_t> samples) noexcept {
    if (Stopped() || !client_ || samples.empty()) {
        return;
    }
    const std::uint64_t before_desync = pairer_.DesyncCount();
    auto paired = left ? pairer_.PushLeft(fs_timestamp_samples, samples)
                       : pairer_.PushRight(fs_timestamp_samples, samples);
    const bool new_desync = pairer_.DesyncCount() > before_desync;
    if (new_desync) {
        bool expected = false;
        if (desync_episode_active_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            EmitRateLimited("osw.recording.lr_desync", &last_desync_emit_);
        }
    } else if (paired.has_value()) {
        desync_episode_active_.store(false, std::memory_order_release);
    }
    if (paired.has_value()) {
        FlushPairedFrame(std::move(*paired));
    }
}

void RecordingRelay::TickLoop() noexcept {
    while (!Stopped()) {
        std::this_thread::sleep_for(kTickInterval);
        if (Stopped()) {
            break;
        }
        const std::uint64_t before_desync = pairer_.DesyncCount();
        auto paired = pairer_.Tick();
        const bool new_desync = pairer_.DesyncCount() > before_desync;
        if (new_desync) {
            bool expected = false;
            if (desync_episode_active_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                EmitRateLimited("osw.recording.lr_desync", &last_desync_emit_);
            }
        } else if (paired.has_value()) {
            desync_episode_active_.store(false, std::memory_order_release);
        }
        if (paired.has_value()) {
            FlushPairedFrame(std::move(*paired));
        }
    }
}

void RecordingRelay::FlushPairedFrame(PairedFrame paired) noexcept {
    if (!client_ || paired.samples_per_channel == 0 || paired.interleaved.empty()) {
        return;
    }

    try {
        std::vector<std::int16_t> samples;
        std::uint32_t channels = 1;
        std::uint32_t proto_channel = 0;

        if (config_.stereo) {
            channels = 2;
            proto_channel = kBothInterleavedChannel;
            samples.reserve(paired.samples_per_channel * 2u);
            for (std::uint32_t i = 0; i < paired.samples_per_channel * 2u; ++i) {
                samples.push_back(ReadInt16Le(paired.interleaved, i));
            }
        } else {
            samples.reserve(paired.samples_per_channel);
            for (std::uint32_t i = 0; i < paired.samples_per_channel; ++i) {
                const std::int16_t left = ReadInt16Le(paired.interleaved, i * 2u);
                const std::int16_t right = ReadInt16Le(paired.interleaved, i * 2u + 1u);
                samples.push_back(AverageSamples(left, right));
            }
        }

        const std::uint64_t seq = client_->NextSeq();
        const std::uint64_t timestamp = client_->AdvanceTimestamp(paired.samples_per_channel);
        AudioFrame frame(
            std::move(samples), config_.sample_rate_hz, channels, seq, timestamp, proto_channel);
        if (!client_->SendAudio(std::move(frame))) {
            EmitRateLimited("osw.recording.send_overflow", &last_overflow_emit_);
        }
    } catch (...) {
        EmitRateLimited("osw.recording.send_overflow", &last_overflow_emit_);
    }
}

void RecordingRelay::EmitRateLimited(const char* subclass,
                                     std::chrono::steady_clock::time_point* last_emit) noexcept {
    if (!subclass || !last_emit) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(emit_mu_);
    if (last_emit->time_since_epoch().count() != 0 && now - *last_emit < kAuditRateLimit) {
        return;
    }
    *last_emit = now;
    (void)osw::audit::EmitSubclass(subclass,
                                   {{"channel_uuid", config_.channel_uuid},
                                    {"stream_id", config_.stream_id},
                                    {"tenant_id", config_.tenant_id}});
}

}  // namespace osw::media

extern "C" switch_bool_t OswRecordingReadTap(switch_media_bug_t* bug,
                                             void* user_data,
                                             switch_abc_type_t type) noexcept {
    const int t = static_cast<int>(type);
    if (t == kAbcTypeInit || t == kAbcTypeClose) {
        return SWITCH_TRUE;
    }
    if (t != kAbcTypeRead) {
        return SWITCH_TRUE;
    }
    auto* relay = static_cast<osw::media::RecordingRelay*>(user_data);
    if (!relay || relay->Stopped()) {
        return SWITCH_TRUE;
    }

#if !defined(OSW_TEST_FS_MOCK)
    switch_frame_t frame{};
    if (osw::raii::fs::MediaBugRead(bug, &frame, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
        !frame.data || frame.datalen == 0) {
        return SWITCH_TRUE;
    }
    const auto samples = MonoSamplesFromFrame(frame);
    relay->PushReadFrame(static_cast<std::uint64_t>(frame.timestamp), samples);
#else
    (void)bug;
#endif
    return SWITCH_TRUE;
}

extern "C" switch_bool_t OswRecordingWriteTap(switch_media_bug_t* bug,
                                              void* user_data,
                                              switch_abc_type_t type) noexcept {
    const int t = static_cast<int>(type);
    if (t == kAbcTypeInit || t == kAbcTypeClose) {
        return SWITCH_TRUE;
    }
    if (t != kAbcTypeWrite) {
        return SWITCH_TRUE;
    }
    auto* relay = static_cast<osw::media::RecordingRelay*>(user_data);
    if (!relay || relay->Stopped()) {
        return SWITCH_TRUE;
    }

#if !defined(OSW_TEST_FS_MOCK)
    switch_frame_t frame{};
    if (osw::raii::fs::MediaBugRead(bug, &frame, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
        !frame.data || frame.datalen == 0) {
        return SWITCH_TRUE;
    }
    const auto samples = MonoSamplesFromFrame(frame);
    relay->PushWriteFrame(static_cast<std::uint64_t>(frame.timestamp), samples);
#else
    (void)bug;
#endif
    return SWITCH_TRUE;
}
