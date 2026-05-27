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
#include <limits>
#include <utility>

#include "osw/observability/audit.h"
#include "osw/observability/log.h"

namespace osw::media {

namespace {

constexpr const char* kSubsystem = "media.tts_playout";

// Rate-limit window for audit events (1 per stream per second).
constexpr std::chrono::seconds kAuditRateWindow{1};

std::uint32_t DurationMsForSamples(std::uint32_t samples,
                                   std::uint32_t sample_rate_hz,
                                   std::uint32_t channels) noexcept {
    if (samples == 0 || sample_rate_hz == 0 || channels == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(samples) * 1000u) /
                                      (static_cast<std::uint64_t>(sample_rate_hz) * channels));
}

std::uint32_t FillRepeatLast(std::int16_t* out,
                             std::uint32_t out_cap_samples,
                             const AudioFrame& last_frame) noexcept {
    const std::uint32_t last_samples = static_cast<std::uint32_t>(last_frame.sample_count());
    if (!out || out_cap_samples == 0 || last_samples == 0) {
        return 0;
    }
    std::uint32_t written = 0;
    while (written < out_cap_samples) {
        const std::uint32_t n = std::min(out_cap_samples - written, last_samples);
        std::memcpy(out + written, last_frame.data(), n * sizeof(std::int16_t));
        written += n;
    }
    return written;
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

    if (!first_push_logged_) {
        first_push_logged_ = true;
        osw::log::Info(kSubsystem,
                       "TtsPlayoutBuffer first push stream_id=%s samples=%zu rate=%u depth_ms=%u "
                       "preroll_ms=%lld",
                       stream_id_.c_str(),
                       queue_.back().sample_count(),
                       queue_.back().sample_rate_hz(),
                       depth_ms_,
                       static_cast<long long>(cfg_.preroll_ms.count()));
    }

    // Drop oldest frames while over high-water mark.
    std::uint64_t dropped = 0;
    while (depth_ms_ > static_cast<std::uint32_t>(cfg_.high_water_ms.count()) &&
           queue_.size() > 1) {
        // If the oldest frame is partially consumed, dropping it must also
        // clear the offset; otherwise the next frame would be read from the
        // middle, creating an audible gap/click.
        front_offset_samples_ = 0;
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
        const std::uint32_t n = out_cap_samples;
        std::memset(out, 0, n * sizeof(std::int16_t));
        if (!first_preroll_silence_logged_) {
            first_preroll_silence_logged_ = true;
            osw::log::Info(kSubsystem,
                           "TtsPlayoutBuffer preroll silence stream_id=%s cap_samples=%u "
                           "depth_ms=%u preroll_ms=%lld",
                           stream_id_.c_str(),
                           out_cap_samples,
                           depth_ms_,
                           static_cast<long long>(cfg_.preroll_ms.count()));
        }
        return n;
    }

    // Case: Buffer has frames — deliver a full callback frame by draining
    // across queued AudioFrames.  Upstream frames may be shorter than the FS
    // ptime; returning a short replacement frame causes RTP packetization
    // jitter, so only EOS-before-any-audio returns 0.
    if (!queue_.empty()) {
        std::uint32_t written = 0;
        while (written < out_cap_samples && !queue_.empty()) {
            AudioFrame& front = queue_.front();
            // W6.5 P2-004 fix: respect front_offset_samples_ so partial-frame
            // consumption preserves the tail across multiple Pop() calls.
            const std::uint32_t total = static_cast<std::uint32_t>(front.sample_count());
            const std::uint32_t avail =
                (front_offset_samples_ >= total) ? 0u : (total - front_offset_samples_);
            if (avail == 0) {
                queue_.pop_front();
                front_offset_samples_ = 0;
                continue;
            }

            const std::uint32_t n = std::min(out_cap_samples - written, avail);
            std::memcpy(out + written,
                        front.data() + front_offset_samples_,
                        n * sizeof(std::int16_t));

            // Keep last frame for kRepeatLast policy (full frame, not slice).
            if (cfg_.underrun == UnderrunPolicy::kRepeatLast) {
                last_frame_ = front;
                has_last_frame_ = true;
            }

            front_offset_samples_ += n;
            written += n;
            if (front_offset_samples_ >= total) {
                queue_.pop_front();
                front_offset_samples_ = 0;
            }
        }
        RecomputeDepth();
        playback_started_.store(true, std::memory_order_relaxed);

        if (!first_pop_logged_) {
            first_pop_logged_ = true;
            osw::log::Info(kSubsystem,
                           "TtsPlayoutBuffer first audio pop stream_id=%s samples=%u "
                           "depth_ms=%u first_sample=%d",
                           stream_id_.c_str(),
                           written,
                           depth_ms_,
                           written > 0 ? static_cast<int>(out[0]) : 0);
        }

        if (written == out_cap_samples) {
            return written;
        }

        // Queue ran dry mid-callback.  Pad the rest so FS sees a stable ptime.
        const std::uint32_t remaining = out_cap_samples - written;
        if (eos_.load(std::memory_order_relaxed)) {
            std::memset(out + written, 0, remaining * sizeof(std::int16_t));
            return out_cap_samples;
        }

        underrun_count_.fetch_add(1, std::memory_order_relaxed);
        if (cfg_.underrun == UnderrunPolicy::kRepeatLast && has_last_frame_) {
            FillRepeatLast(out + written, remaining, last_frame_);
        } else {
            std::memset(out + written, 0, remaining * sizeof(std::int16_t));
        }
        if (!first_underrun_logged_) {
            first_underrun_logged_ = true;
            osw::log::Warn(kSubsystem,
                           "TtsPlayoutBuffer underrun stream_id=%s samples_silenced=%u "
                           "depth_ms=%u",
                           stream_id_.c_str(),
                           remaining,
                           depth_ms_);
        }
        EmitUnderrunEvent(remaining, depth_ms_);
        return out_cap_samples;
    }

    // Case: EOS, buffer just drained — return 0 cleanly.
    if (eos_.load(std::memory_order_relaxed)) {
        return 0;
    }

    // Case: Buffer empty, playback was started, not EOS — underrun.
    if (!playback_started_.load(std::memory_order_relaxed)) {
        // Playback never started; treat as priming silence (shouldn't normally
        // reach here after preroll, but be defensive).
        const std::uint32_t n = out_cap_samples;
        std::memset(out, 0, n * sizeof(std::int16_t));
        return n;
    }

    // Real underrun.
    underrun_count_.fetch_add(1, std::memory_order_relaxed);

    std::uint32_t n = 0;
    if (cfg_.underrun == UnderrunPolicy::kRepeatLast && has_last_frame_) {
        n = FillRepeatLast(out, out_cap_samples, last_frame_);
    } else {
        n = out_cap_samples;
        std::memset(out, 0, n * sizeof(std::int16_t));
    }

    if (!first_underrun_logged_) {
        first_underrun_logged_ = true;
        osw::log::Warn(kSubsystem,
                       "TtsPlayoutBuffer underrun stream_id=%s samples_silenced=%u depth_ms=%u",
                       stream_id_.c_str(),
                       n,
                       depth_ms_);
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
    // W6.5 P2-005 fix: serialise label writes under mu_ so the
    // StreamClient reader thread (which calls Push() → potentially
    // EmitOverrunEvent under mu_, reading stream_id_) doesn't race
    // with the handler thread setting the labels after Open() returned.
    std::lock_guard<std::mutex> lk(mu_);
    stream_id_ = std::move(stream_id);
}

void TtsPlayoutBuffer::SetTenantId(std::string tenant_id) noexcept {
    std::lock_guard<std::mutex> lk(mu_);  // W6.5 P2-005 fix
    tenant_id_ = std::move(tenant_id);
}

// Internal helpers — called with mu_ held.

void TtsPlayoutBuffer::RecomputeDepth() noexcept {
    std::uint64_t total = 0;
    bool first = true;
    for (const auto& f : queue_) {
        std::uint32_t samples = static_cast<std::uint32_t>(f.sample_count());
        if (first) {
            samples = (front_offset_samples_ >= samples) ? 0u : (samples - front_offset_samples_);
            first = false;
        }
        total += DurationMsForSamples(samples, f.sample_rate_hz(), f.channels());
    }
    depth_ms_ =
        static_cast<std::uint32_t>(
            std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
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
