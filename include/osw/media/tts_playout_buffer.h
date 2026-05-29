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
#include <string>
#include <vector>

#include "osw/media/audio_frame.h"

namespace osw::observability::prometheus {
class Histogram;
class Counter;
}  // namespace osw::observability::prometheus

namespace osw::media {

/// Jitter buffer between StreamClient rx queue and the FS WRITE_REPLACE
/// media bug callback.
class TtsPlayoutBuffer {
  public:
    enum class UnderrunPolicy {
        kSilence,     ///< default — emit zeroed samples
        kRepeatLast,  ///< copy the last 20 ms frame; better for music
    };

    struct Config {
        std::chrono::milliseconds target_ms;      ///< target buffer depth, e.g. 1000
        std::chrono::milliseconds preroll_ms;     ///< wait until this depth before playing
        std::chrono::milliseconds high_water_ms;  ///< drop oldest when depth exceeds this
        UnderrunPolicy underrun = UnderrunPolicy::kSilence;
        std::uint32_t channel_sample_rate_hz;  ///< 8000 or 16000 (matches channel)
        std::uint32_t channels = 1;            ///< 1 (mono) for V1
    };

    explicit TtsPlayoutBuffer(Config cfg) noexcept;
    ~TtsPlayoutBuffer() noexcept;

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
    /// Writes one stable replacement frame of L16 into out and returns the
    /// number of samples written. Except for empty-buffer EOS, Pop() fills
    /// out_cap_samples by draining across queued frames and padding with the
    /// underrun policy if the queue runs dry. Behaviour:
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
    [[nodiscard]] bool FirstPushObserved() const noexcept;
    [[nodiscard]] bool FirstAudioPopObserved() const noexcept;

    // --- Identifiers for Prometheus labels + audit events ----------------
    void SetStreamId(std::string stream_id) noexcept;
    void SetTenantId(std::string tenant_id) noexcept;

    /// Wire Prometheus metrics (non-owning; may be null in tests). The
    /// first-audio latency is Observed once at the first real audio pop
    /// (true text->audible, includes pre-roll); underruns are counted as
    /// they occur. Set after construction by the StartTts/Voicebot handler.
    void SetMetrics(observability::prometheus::Histogram* first_audio_latency,
                    observability::prometheus::Counter* underrun_total) noexcept;

  private:
    Config cfg_;

    mutable std::mutex mu_;
    std::deque<AudioFrame> queue_;
    /// W6.5 P2-004 fix: offset (in samples) into queue_.front() if the
    /// previous Pop() consumed only part of it.  Pop() drains from this
    /// offset; pops the front frame only when the offset reaches
    /// sample_count().  Previous code dropped the unconsumed tail every
    /// Pop, which caused audible truncation when the TTS service sent
    /// frames larger than one FS ptime (20 ms).
    std::uint32_t front_offset_samples_ = 0;
    /// Cached sum of queue_ frame durations in ms (recomputed on push/pop).
    std::uint32_t depth_ms_ = 0;

    // Last frame emitted (for kRepeatLast policy). Protected by mu_.
    AudioFrame last_frame_;
    bool has_last_frame_ = false;
    bool first_push_logged_ = false;
    bool first_pop_logged_ = false;
    bool first_preroll_silence_logged_ = false;
    bool first_underrun_logged_ = false;
    std::chrono::steady_clock::time_point created_at_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point first_push_at_{};
    std::chrono::steady_clock::time_point preroll_reached_at_{};

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

    // Prometheus metrics (non-owning, set by handler after construction; null
    // in tests / when metrics disabled).
    observability::prometheus::Histogram* first_audio_latency_ = nullptr;
    observability::prometheus::Counter* underrun_total_ = nullptr;

    // Internal helpers — called with mu_ held.
    void RecomputeDepth() noexcept;
    void EmitUnderrunEvent(std::uint32_t samples_silenced, std::uint64_t depth_ms) noexcept;
    void EmitOverrunEvent(std::uint64_t frames_dropped, std::uint64_t depth_ms) noexcept;
    void InitDebugDumpLocked() noexcept;
    void DebugCaptureSamples(std::vector<std::int16_t>& dst,
                             const std::int16_t* samples,
                             std::uint32_t sample_count) noexcept;
    void DebugFlushDumpsLocked() noexcept;

    bool debug_audio_enabled_ = false;
    bool debug_audio_checked_ = false;
    bool debug_audio_flushed_ = false;
    std::size_t debug_audio_max_samples_ = 0;
    std::string debug_audio_dir_;
    std::vector<std::int16_t> debug_push_samples_;
    std::vector<std::int16_t> debug_pop_samples_;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_TTS_PLAYOUT_BUFFER_H_
