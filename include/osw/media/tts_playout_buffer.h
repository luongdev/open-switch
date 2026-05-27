/*
 * include/osw/media/tts_playout_buffer.h
 *
 * osw::media::TtsPlayoutBuffer — jitter buffer between the StreamClient
 * reader thread (producer) and the FreeSWITCH WRITE_REPLACE bug callback
 * (consumer). Decouples the bursty gRPC delivery cadence of TTS/voicebot
 * engines from the 20 ms ptime cadence of the FS media thread.
 *
 * Thread model:
 *   - One producer:  StreamClient reader thread calls Push().
 *   - One consumer:  FS media thread calls Pop() from the bug callback.
 *   - Snapshot readers: arbitrary threads (Prometheus scrape, Health).
 *     Exposed counters use std::memory_order_relaxed atomics.
 *
 * The internal queue is std::deque<AudioFrame> under a single std::mutex.
 * No condvar is used — the FS callback must never block. Capacity is
 * bounded by depth-in-time (not frame count) because frames may have
 * variable sample counts after resampling.
 *
 * See openspec/changes/core-module-v1/implementation/W6-track-C-handlers.md
 * §"TtsPlayoutBuffer" for the full design.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_TTS_PLAYOUT_BUFFER_H_
#define OSW_MEDIA_TTS_PLAYOUT_BUFFER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

#include "osw/media/audio_frame.h"

namespace osw::media {

/// Jitter buffer between StreamClient rx queue and the FS WRITE_REPLACE
/// media bug callback.
class TtsPlayoutBuffer {
  public:
    enum class UnderrunPolicy {
        kSilence,    ///< default — emit zeroed samples
        kRepeatLast, ///< copy the last 20 ms frame; better for music
    };

    struct Config {
        std::chrono::milliseconds target_ms;     ///< target buffer depth, e.g. 1000
        std::chrono::milliseconds preroll_ms;    ///< wait until this depth before playing
        std::chrono::milliseconds high_water_ms; ///< drop oldest when depth exceeds this
        UnderrunPolicy underrun = UnderrunPolicy::kSilence;
        std::uint32_t channel_sample_rate_hz;    ///< 8000 or 16000 (matches channel)
        std::uint32_t channels = 1;              ///< 1 (mono) for V1
    };

    explicit TtsPlayoutBuffer(Config cfg) noexcept;
    ~TtsPlayoutBuffer() noexcept = default;

    TtsPlayoutBuffer(const TtsPlayoutBuffer&) = delete;
    TtsPlayoutBuffer& operator=(const TtsPlayoutBuffer&) = delete;
    TtsPlayoutBuffer(TtsPlayoutBuffer&&) = delete;
    TtsPlayoutBuffer& operator=(TtsPlayoutBuffer&&) = delete;

    /// Producer (StreamClient reader thread): push a server-sent frame.
    /// If after Push() depth > high_water_ms, drop oldest frame(s) until
    /// depth == high_water_ms; each dropped frame increments overrun
    /// counter and emits one osw.media.tts.overrun audit event (rate-
    /// limited to 1/s per stream).
    void Push(AudioFrame frame) noexcept;

    /// Consumer (FS write_replace bug callback, on the FS media thread).
    /// Writes up to out_cap_samples of L16 into out; returns the
    /// number of samples actually written. Behaviour:
    ///   - Pre-roll not yet reached AND not end-of-stream: write silence
    ///     (zeros), return count. Do NOT signal underrun (we're priming).
    ///   - Buffer has at least one frame: pop oldest, copy samples.
    ///   - Buffer empty AND end-of-stream: return 0 (caller leaves frame
    ///     untouched; FS sends silence naturally).
    ///   - Buffer empty AND playback started AND not EOS: underrun —
    ///     write per UnderrunPolicy; increment underrun counter; emit
    ///     osw.media.tts.underrun (rate-limited 1/s per stream).
    std::uint32_t Pop(std::int16_t* out, std::uint32_t out_cap_samples) noexcept;

    /// Producer: signal the server side has half-closed. Pop() will drain
    /// remaining frames, then return 0 cleanly (no underrun metric after EOS).
    void SignalEndOfStream() noexcept;

    // --- Snapshot accessors (thread-safe, relaxed atomics) ---------------
    [[nodiscard]] std::chrono::milliseconds CurrentDepth() const noexcept;
    [[nodiscard]] std::uint64_t UnderrunCount() const noexcept;
    [[nodiscard]] std::uint64_t OverrunCount() const noexcept;
    [[nodiscard]] bool PrerollReached() const noexcept;
    [[nodiscard]] bool EndOfStream() const noexcept;

    // --- Identifiers for Prometheus labels + audit events ----------------
    void SetStreamId(std::string stream_id) noexcept;
    void SetTenantId(std::string tenant_id) noexcept;

  private:
    Config cfg_;

    mutable std::mutex mu_;
    std::deque<AudioFrame> queue_;
    /// Cached sum of queue_ frame durations in ms (recomputed on push/pop).
    std::uint32_t depth_ms_ = 0;

    // Last frame emitted (for kRepeatLast policy). Protected by mu_.
    AudioFrame last_frame_;
    bool has_last_frame_ = false;

    // Atomics for snapshot readers (Prometheus, Health) — no mu_ needed.
    std::atomic<bool> preroll_reached_{false};
    std::atomic<bool> eos_{false};
    std::atomic<std::uint64_t> underrun_count_{0};
    std::atomic<std::uint64_t> overrun_count_{0};

    // Once Pop() has ever returned real samples, playback has started.
    // Underruns are only counted after playback starts (not during prime).
    std::atomic<bool> playback_started_{false};

    // Rate-limiting for audit events: track last-emit time per event type.
    // Guarded by mu_ (producer/consumer already hold mu_).
    std::chrono::steady_clock::time_point last_underrun_emit_{};
    std::chrono::steady_clock::time_point last_overrun_emit_{};
    std::uint64_t underrun_suppressed_ = 0;
    std::uint64_t overrun_suppressed_ = 0;

    // Prometheus gauge (non-owning, set by handler after construction).
    // May be null when running in tests without a registry.

    // Labels for audit/metrics (set after construction).
    std::string stream_id_;
    std::string tenant_id_;

    // Internal helpers — called with mu_ held.
    void RecomputeDepth() noexcept;
    void EmitUnderrunEvent(std::uint32_t samples_silenced, std::uint64_t depth_ms) noexcept;
    void EmitOverrunEvent(std::uint64_t frames_dropped, std::uint64_t depth_ms) noexcept;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_TTS_PLAYOUT_BUFFER_H_
