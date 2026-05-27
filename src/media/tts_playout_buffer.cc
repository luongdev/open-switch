/*
 * src/media/tts_playout_buffer.cc
 *
 * osw::media::TtsPlayoutBuffer implementation.
 *
 * Thread model: single-producer (StreamClient reader thread) /
 * single-consumer (FS media thread). Internal deque under one mutex;
 * no condvar (FS callback must never block).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/tts_playout_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "osw/observability/audit.h"
#include "osw/observability/log.h"

namespace osw::media {

namespace {

constexpr const char* kSubsystem = "media.tts_playout";

// Rate-limit window for audit events (1 per stream per second).
constexpr std::chrono::seconds kAuditRateWindow{1};

// Helper: compute ptime ms from a frame with given sample_rate_hz.
inline std::uint32_t FrameDurationMs(const AudioFrame& f) noexcept {
    return f.duration_ms();
}

}  // namespace

TtsPlayoutBuffer::TtsPlayoutBuffer(Config cfg) noexcept : cfg_(cfg) {}

void TtsPlayoutBuffer::Push(AudioFrame frame) noexcept {
    if (frame.sample_count() == 0) {
        return;
    }
    std::lock_guard<std::mutex> g(mu_);

    queue_.push_back(std::move(frame));
    RecomputeDepth();

    // Drop oldest frames while over high-water mark.
    std::uint64_t dropped = 0;
    while (depth_ms_ > static_cast<std::uint32_t>(cfg_.high_water_ms.count()) &&
           queue_.size() > 1) {
        queue_.pop_front();
        ++dropped;
        RecomputeDepth();
    }

    if (dropped > 0) {
        const std::uint64_t new_count =
            overrun_count_.fetch_add(dropped, std::memory_order_relaxed) + dropped;
        (void)new_count;
        EmitOverrunEvent(dropped, depth_ms_);
    }

    // Transition to PrerollReached if threshold crossed.
    if (!preroll_reached_.load(std::memory_order_relaxed) &&
        depth_ms_ >= static_cast<std::uint32_t>(cfg_.preroll_ms.count())) {
        preroll_reached_.store(true, std::memory_order_relaxed);
    }
}

std::uint32_t TtsPlayoutBuffer::Pop(std::int16_t* out, std::uint32_t out_cap_samples) noexcept {
    if (!out || out_cap_samples == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> g(mu_);

    // Case: EOS and buffer empty.
    if (eos_.load(std::memory_order_relaxed) && queue_.empty()) {
        return 0;
    }

    // Case: Pre-roll not yet reached (still priming) — emit silence.
    if (!preroll_reached_.load(std::memory_order_relaxed) &&
        !eos_.load(std::memory_order_relaxed)) {
        const std::uint32_t n = std::min(out_cap_samples,
                                         cfg_.channel_sample_rate_hz / 50);  // 20ms worth
        std::memset(out, 0, n * sizeof(std::int16_t));
        return n;
    }

    // Case: Buffer has frames — deliver.
    if (!queue_.empty()) {
        AudioFrame& front = queue_.front();
        const std::uint32_t avail = static_cast<std::uint32_t>(front.sample_count());
        const std::uint32_t n = std::min(out_cap_samples, avail);
        std::memcpy(out, front.data(), n * sizeof(std::int16_t));

        // Keep last frame for kRepeatLast policy.
        if (cfg_.underrun == UnderrunPolicy::kRepeatLast) {
            last_frame_ = front;
            has_last_frame_ = true;
        }

        queue_.pop_front();
        RecomputeDepth();
        playback_started_.store(true, std::memory_order_relaxed);
        return n;
    }

    // Case: EOS, buffer just drained — return 0 cleanly.
    if (eos_.load(std::memory_order_relaxed)) {
        return 0;
    }

    // Case: Buffer empty, playback was started, not EOS — underrun.
    if (!playback_started_.load(std::memory_order_relaxed)) {
        // Playback never started; treat as priming silence (shouldn't normally
        // reach here after preroll, but be defensive).
        const std::uint32_t n = std::min(out_cap_samples, cfg_.channel_sample_rate_hz / 50);
        std::memset(out, 0, n * sizeof(std::int16_t));
        return n;
    }

    // Real underrun.
    underrun_count_.fetch_add(1, std::memory_order_relaxed);

    std::uint32_t n = 0;
    if (cfg_.underrun == UnderrunPolicy::kRepeatLast && has_last_frame_) {
        const std::uint32_t avail = static_cast<std::uint32_t>(last_frame_.sample_count());
        n = std::min(out_cap_samples, avail);
        std::memcpy(out, last_frame_.data(), n * sizeof(std::int16_t));
    } else {
        n = std::min(out_cap_samples, cfg_.channel_sample_rate_hz / 50);
        std::memset(out, 0, n * sizeof(std::int16_t));
    }

    EmitUnderrunEvent(n, depth_ms_);
    return n;
}

void TtsPlayoutBuffer::SignalEndOfStream() noexcept {
    eos_.store(true, std::memory_order_relaxed);
    osw::log::Debug(kSubsystem, "TtsPlayoutBuffer EOS stream_id=%s", stream_id_.c_str());
}

std::chrono::milliseconds TtsPlayoutBuffer::CurrentDepth() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return std::chrono::milliseconds(depth_ms_);
}

std::uint64_t TtsPlayoutBuffer::UnderrunCount() const noexcept {
    return underrun_count_.load(std::memory_order_relaxed);
}

std::uint64_t TtsPlayoutBuffer::OverrunCount() const noexcept {
    return overrun_count_.load(std::memory_order_relaxed);
}

bool TtsPlayoutBuffer::PrerollReached() const noexcept {
    return preroll_reached_.load(std::memory_order_relaxed);
}

bool TtsPlayoutBuffer::EndOfStream() const noexcept {
    return eos_.load(std::memory_order_relaxed);
}

void TtsPlayoutBuffer::SetStreamId(std::string stream_id) noexcept {
    stream_id_ = std::move(stream_id);
}

void TtsPlayoutBuffer::SetTenantId(std::string tenant_id) noexcept {
    tenant_id_ = std::move(tenant_id);
}

// Internal helpers — called with mu_ held.

void TtsPlayoutBuffer::RecomputeDepth() noexcept {
    std::uint32_t total = 0;
    for (const auto& f : queue_) {
        total += f.duration_ms();
    }
    depth_ms_ = total;
}

void TtsPlayoutBuffer::EmitUnderrunEvent(std::uint32_t samples_silenced,
                                         std::uint64_t depth_ms) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_underrun_emit_ < kAuditRateWindow) {
        ++underrun_suppressed_;
        return;
    }
    last_underrun_emit_ = now;
    const std::uint64_t suppressed = underrun_suppressed_;
    underrun_suppressed_ = 0;

    osw::audit::Emit("media.tts.underrun",
                     {{"stream_id", stream_id_},
                      {"tenant_id", tenant_id_},
                      {"depth_ms", std::to_string(depth_ms)},
                      {"samples_silenced", std::to_string(samples_silenced)},
                      {"suppressed_count", std::to_string(suppressed)}});
}

void TtsPlayoutBuffer::EmitOverrunEvent(std::uint64_t frames_dropped,
                                        std::uint64_t depth_ms) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_overrun_emit_ < kAuditRateWindow) {
        ++overrun_suppressed_;
        return;
    }
    last_overrun_emit_ = now;
    const std::uint64_t suppressed = overrun_suppressed_;
    overrun_suppressed_ = 0;

    osw::audit::Emit("media.tts.overrun",
                     {{"stream_id", stream_id_},
                      {"tenant_id", tenant_id_},
                      {"depth_ms", std::to_string(depth_ms)},
                      {"frames_dropped", std::to_string(frames_dropped)},
                      {"suppressed_count", std::to_string(suppressed)}});
}

}  // namespace osw::media
