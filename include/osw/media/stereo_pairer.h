/*
 * include/osw/media/stereo_pairer.h
 *
 * osw::media::StereoFramePairer — W7 Track B timestamp pairer for
 * module-owned stereo recording relay. Pairs caller mic (left) and
 * post-injection write audio (right), emits BOTH_INTERLEAVED PCM_S16LE.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_STEREO_PAIRER_H_
#define OSW_MEDIA_STEREO_PAIRER_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace osw::media {

struct PairedFrame {
    std::vector<std::uint8_t> interleaved;  ///< [L0 R0 L1 R1 ...] int16 LE
    std::uint32_t samples_per_channel = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t seq = 0;
};

class StereoFramePairer {
  public:
    struct Config {
        std::uint32_t sample_rate_hz = 8000;
        std::uint32_t desync_warn_ms = 5;
        std::uint32_t desync_timeout_ms = 25;
    };

    explicit StereoFramePairer(Config cfg);
    ~StereoFramePairer() noexcept;

    StereoFramePairer(const StereoFramePairer&) = delete;
    StereoFramePairer& operator=(const StereoFramePairer&) = delete;
    StereoFramePairer(StereoFramePairer&&) = delete;
    StereoFramePairer& operator=(StereoFramePairer&&) = delete;

    /// Push a left-channel caller frame. Returns a stereo frame when the
    /// matching right frame is available or when timeout forces silence-fill.
    [[nodiscard]] std::optional<PairedFrame> PushLeft(
        std::uint64_t fs_timestamp_samples, std::span<const std::int16_t> samples) noexcept;

    /// Push a right-channel post-injection write frame. Symmetric to PushLeft.
    [[nodiscard]] std::optional<PairedFrame> PushRight(
        std::uint64_t fs_timestamp_samples, std::span<const std::int16_t> samples) noexcept;

    /// Timer safety net: flush a one-sided head frame with silence after
    /// desync_timeout_ms of wall-clock wait.
    [[nodiscard]] std::optional<PairedFrame> Tick() noexcept;

    [[nodiscard]] std::uint64_t DesyncCount() const noexcept;
    [[nodiscard]] std::uint64_t PairedCount() const noexcept;
    [[nodiscard]] std::uint64_t WarnCount() const noexcept;
    [[nodiscard]] std::uint64_t DroppedCount() const noexcept;

  private:
    struct PendingFrame {
        std::uint64_t fs_timestamp_samples = 0;
        std::chrono::steady_clock::time_point arrived_at{};
        std::vector<std::int16_t> samples;
    };

    enum class Side {
        kLeft,
        kRight,
    };

    // Keep enough headroom for the max 100 ms desync timeout at 10 ms ptime
    // plus jitter. Smaller rings can drop the oldest one-sided frame before
    // timeout-driven silence fill is allowed to fire.
    static constexpr std::size_t kRingCapacity = 16;

    void EnqueueLocked(Side side, PendingFrame frame) noexcept;
    [[nodiscard]] PendingFrame PopLocked(Side side) noexcept;
    [[nodiscard]] const PendingFrame& FrontLocked(Side side) const noexcept;

    [[nodiscard]] std::optional<PairedFrame> TryEmitLocked(
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::optional<PairedFrame> TryEmitExpiredLocked(
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] PairedFrame MakePairedFrameLocked(std::span<const std::int16_t> left,
                                                    std::span<const std::int16_t> right);
    [[nodiscard]] PairedFrame EmitSilenceFillLocked(Side real_side);
    void RecordWarnLocked(std::uint64_t diff_samples) noexcept;
    void RecordTimeoutLocked(Side real_side, std::uint64_t timestamp_samples) noexcept;

    [[nodiscard]] std::uint64_t WarnThresholdSamplesLocked() const noexcept;
    [[nodiscard]] std::uint64_t TimeoutThresholdSamplesLocked() const noexcept;

    Config cfg_;
    std::array<PendingFrame, kRingCapacity> left_ring_{};
    std::array<PendingFrame, kRingCapacity> right_ring_{};
    std::size_t left_head_ = 0;
    std::size_t left_tail_ = 0;
    std::size_t left_count_ = 0;
    std::size_t right_head_ = 0;
    std::size_t right_tail_ = 0;
    std::size_t right_count_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t paired_count_ = 0;
    std::uint64_t desync_count_ = 0;
    std::uint64_t warn_count_ = 0;
    std::uint64_t dropped_count_ = 0;
    mutable std::mutex mu_;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_STEREO_PAIRER_H_
