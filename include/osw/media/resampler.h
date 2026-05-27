/*
 * include/osw/media/resampler.h
 *
 * osw::media::Resampler — thin wrapper around switch_audio_resampler_t.
 *
 * Reuses internal FIR state across calls (mandatory — see
 * FREESWITCH-FACTS FF-033). Not thread-safe; owned per StreamClient.
 *
 * V1 policy: only 8 kHz ↔ 16 kHz conversion pairs are supported.
 * Any other (from_hz, to_hz) pair returns nullptr from Create(); the
 * caller falls back to no-op (raw frame forwarding) and logs a warning.
 * The FS API itself accepts any rate; the restriction is policy, not
 * a FS limitation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_RESAMPLER_H_
#define OSW_MEDIA_RESAMPLER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace osw::media {

/// Thin wrapper around switch_audio_resampler_t.  Reuses internal FIR
/// state across calls — DO NOT destroy and re-create between frames.
class Resampler {
  public:
    /// Allowed pairs in V1: (8000, 16000) and (16000, 8000).
    /// (8000, 8000) is also accepted as a degenerate pass-through.
    /// Any other pair returns nullptr.
    static std::unique_ptr<Resampler> Create(int from_hz, int to_hz) noexcept;

    ~Resampler() noexcept;
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&) noexcept = delete;
    Resampler& operator=(Resampler&&) noexcept = delete;

    /// Resample `in_samples` samples from `in` into `out` (capacity =
    /// `out_cap` samples).  Returns the number of output samples written.
    /// Returns 0 on FS-internal error or if out_cap is too small.
    std::size_t Process(const std::int16_t* in,
                        std::size_t in_samples,
                        std::int16_t* out,
                        std::size_t out_cap) noexcept;

    [[nodiscard]] int from_hz() const noexcept { return from_hz_; }
    [[nodiscard]] int to_hz() const noexcept { return to_hz_; }

  private:
    // Stored as void* to avoid pulling switch_audio_resampler_t into
    // every consumer's translation unit.  resampler.cc casts back to
    // switch_audio_resampler_t* which it includes via <switch_resample.h>.
    explicit Resampler(void* res, int from_hz, int to_hz) noexcept;

    void* resampler_ = nullptr;  ///< actually switch_audio_resampler_t*
    int from_hz_ = 0;
    int to_hz_ = 0;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_RESAMPLER_H_
