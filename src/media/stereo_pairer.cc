/*
 * src/media/stereo_pairer.cc
 *
 * Implementation of osw::media::StereoFramePairer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/stereo_pairer.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "osw/observability/log.h"

namespace osw::media {

namespace {

constexpr const char* kSubsystem = "media.stereo_pairer";

std::uint64_t DiffSamples(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

void AppendInt16Le(std::vector<std::uint8_t>& out, std::int16_t sample) {
    const auto value = static_cast<std::uint16_t>(sample);
    out.push_back(static_cast<std::uint8_t>(value & 0x00ffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0x00ffu));
}

}  // namespace

StereoFramePairer::StereoFramePairer(Config cfg) : cfg_(cfg) {}

StereoFramePairer::~StereoFramePairer() noexcept = default;

std::optional<PairedFrame> StereoFramePairer::PushLeft(
    std::uint64_t fs_timestamp_samples, std::span<const std::int16_t> samples) noexcept {
    try {
        PendingFrame frame;
        frame.fs_timestamp_samples = fs_timestamp_samples;
        frame.arrived_at = std::chrono::steady_clock::now();
        frame.samples.assign(samples.begin(), samples.end());

        std::lock_guard<std::mutex> g(mu_);
        EnqueueLocked(Side::kLeft, std::move(frame));
        return TryEmitLocked(std::chrono::steady_clock::now());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<PairedFrame> StereoFramePairer::PushRight(
    std::uint64_t fs_timestamp_samples, std::span<const std::int16_t> samples) noexcept {
    try {
        PendingFrame frame;
        frame.fs_timestamp_samples = fs_timestamp_samples;
        frame.arrived_at = std::chrono::steady_clock::now();
        frame.samples.assign(samples.begin(), samples.end());

        std::lock_guard<std::mutex> g(mu_);
        EnqueueLocked(Side::kRight, std::move(frame));
        return TryEmitLocked(std::chrono::steady_clock::now());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<PairedFrame> StereoFramePairer::Tick() noexcept {
    try {
        std::lock_guard<std::mutex> g(mu_);
        return TryEmitLocked(std::chrono::steady_clock::now());
    } catch (...) {
        return std::nullopt;
    }
}

std::uint64_t StereoFramePairer::DesyncCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return desync_count_;
}

std::uint64_t StereoFramePairer::PairedCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return paired_count_;
}

std::uint64_t StereoFramePairer::WarnCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return warn_count_;
}

std::uint64_t StereoFramePairer::DroppedCount() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return dropped_count_;
}

void StereoFramePairer::EnqueueLocked(Side side, PendingFrame frame) noexcept {
    auto& ring = side == Side::kLeft ? left_ring_ : right_ring_;
    auto& head = side == Side::kLeft ? left_head_ : right_head_;
    auto& tail = side == Side::kLeft ? left_tail_ : right_tail_;
    auto& count = side == Side::kLeft ? left_count_ : right_count_;

    if (count == kRingCapacity) {
        ring[tail] = PendingFrame{};
        tail = (tail + 1u) % kRingCapacity;
        --count;
        ++dropped_count_;
    }

    ring[head] = std::move(frame);
    head = (head + 1u) % kRingCapacity;
    ++count;
}

StereoFramePairer::PendingFrame StereoFramePairer::PopLocked(Side side) noexcept {
    auto& ring = side == Side::kLeft ? left_ring_ : right_ring_;
    auto& tail = side == Side::kLeft ? left_tail_ : right_tail_;
    auto& count = side == Side::kLeft ? left_count_ : right_count_;

    PendingFrame out = std::move(ring[tail]);
    ring[tail] = PendingFrame{};
    tail = (tail + 1u) % kRingCapacity;
    --count;
    return out;
}

const StereoFramePairer::PendingFrame& StereoFramePairer::FrontLocked(Side side) const noexcept {
    return side == Side::kLeft ? left_ring_[left_tail_] : right_ring_[right_tail_];
}

std::optional<PairedFrame> StereoFramePairer::TryEmitLocked(
    std::chrono::steady_clock::time_point now) {
    if (left_count_ > 0 && right_count_ > 0) {
        const auto& left = FrontLocked(Side::kLeft);
        const auto& right = FrontLocked(Side::kRight);
        const std::uint64_t diff =
            DiffSamples(left.fs_timestamp_samples, right.fs_timestamp_samples);

        if (diff > TimeoutThresholdSamplesLocked()) {
            return EmitSilenceFillLocked(left.fs_timestamp_samples < right.fs_timestamp_samples
                                             ? Side::kLeft
                                             : Side::kRight);
        }

        PendingFrame left_frame = PopLocked(Side::kLeft);
        PendingFrame right_frame = PopLocked(Side::kRight);

        const std::uint64_t warn_threshold = WarnThresholdSamplesLocked();
        if (diff > 0 && (warn_threshold == 0 || diff >= warn_threshold)) {
            RecordWarnLocked(diff);
        }

        return MakePairedFrameLocked(left_frame.samples, right_frame.samples);
    }

    return TryEmitExpiredLocked(now);
}

std::optional<PairedFrame> StereoFramePairer::TryEmitExpiredLocked(
    std::chrono::steady_clock::time_point now) {
    const auto timeout = std::chrono::milliseconds(cfg_.desync_timeout_ms);

    if (left_count_ > 0) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - FrontLocked(Side::kLeft).arrived_at);
        if (waited > timeout) {
            return EmitSilenceFillLocked(Side::kLeft);
        }
    }

    if (right_count_ > 0) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - FrontLocked(Side::kRight).arrived_at);
        if (waited > timeout) {
            return EmitSilenceFillLocked(Side::kRight);
        }
    }

    return std::nullopt;
}

PairedFrame StereoFramePairer::MakePairedFrameLocked(std::span<const std::int16_t> left,
                                                     std::span<const std::int16_t> right) {
    const std::size_t samples = std::max(left.size(), right.size());

    PairedFrame out;
    out.samples_per_channel = static_cast<std::uint32_t>(samples);
    out.sample_rate_hz = cfg_.sample_rate_hz;
    out.seq = seq_++;
    out.interleaved.reserve(samples * 2u * sizeof(std::int16_t));

    for (std::size_t i = 0; i < samples; ++i) {
        AppendInt16Le(out.interleaved, i < left.size() ? left[i] : 0);
        AppendInt16Le(out.interleaved, i < right.size() ? right[i] : 0);
    }

    ++paired_count_;
    return out;
}

PairedFrame StereoFramePairer::EmitSilenceFillLocked(Side real_side) {
    PendingFrame frame = PopLocked(real_side);
    RecordTimeoutLocked(real_side, frame.fs_timestamp_samples);

    if (real_side == Side::kLeft) {
        return MakePairedFrameLocked(frame.samples, {});
    }
    return MakePairedFrameLocked({}, frame.samples);
}

void StereoFramePairer::RecordWarnLocked(std::uint64_t diff_samples) noexcept {
    ++warn_count_;
    osw::log::Warn(kSubsystem,
                   "event=osw.recording.lr_warn diff_samples=%llu warn_threshold_samples=%llu "
                   "sample_rate_hz=%u",
                   static_cast<unsigned long long>(diff_samples),
                   static_cast<unsigned long long>(WarnThresholdSamplesLocked()),
                   cfg_.sample_rate_hz);
}

void StereoFramePairer::RecordTimeoutLocked(Side real_side,
                                            std::uint64_t timestamp_samples) noexcept {
    ++desync_count_;
    ++warn_count_;
    const char* side_name = real_side == Side::kLeft ? "left" : "right";
    osw::log::Warn(kSubsystem,
                   "event=osw.recording.lr_desync real_side=%s timestamp_samples=%llu "
                   "desync_count=%llu",
                   side_name,
                   static_cast<unsigned long long>(timestamp_samples),
                   static_cast<unsigned long long>(desync_count_));
}

std::uint64_t StereoFramePairer::WarnThresholdSamplesLocked() const noexcept {
    return (static_cast<std::uint64_t>(cfg_.desync_warn_ms) *
            static_cast<std::uint64_t>(cfg_.sample_rate_hz)) /
           1000u;
}

std::uint64_t StereoFramePairer::TimeoutThresholdSamplesLocked() const noexcept {
    return (static_cast<std::uint64_t>(cfg_.desync_timeout_ms) *
            static_cast<std::uint64_t>(cfg_.sample_rate_hz)) /
           1000u;
}

}  // namespace osw::media
