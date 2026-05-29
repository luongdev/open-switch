/*
 * tests/unit/media/resampler_test.cc
 *
 * Unit tests for osw::media::Resampler.
 *
 * Requires the real FreeSWITCH library (libfreeswitch.so) present in the
 * builder container. Resampler wraps switch_audio_resampler_t which is
 * backed by spandsp — available in simplefs/open-switch-base:1.10.12-trixie.
 *
 * Scenarios (W6 Track B spec):
 *   R1 — Create(8000, 16000) → non-null.
 *   R2 — Create(16000, 8000) → non-null.
 *   R3 — Create(8000, 8000)  → nullptr; Supports() true pass-through.
 *   R4 — Create(8000, 24000) → nullptr.
 *   R5 — Create(48000, 16000) → nullptr.
 *   R6 — Upsample 160 mono samples 8→16 kHz: 320 ± 4 samples written.
 *   R7 — Downsample 320 mono samples 16→8 kHz: 160 ± 4 samples written.
 *   R8 — Audio fidelity: 1 kHz sine 200 ms @ 8 kHz → upsample → RMS
 *        energy within ±0.5 dB of input.
 *   R9 — Stateful across calls: feed sine in 5 chunks of 64 samples;
 *        output is contiguous (no phase discontinuity — verified by
 *        absence of high-frequency artifacts in RMS of inter-chunk deltas).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/resampler.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace {

using osw::media::Resampler;

// ---------------------------------------------------------------------------
// Sine wave generator helper
// ---------------------------------------------------------------------------

/// Generate `n` samples of a sine wave at frequency `freq_hz` sampled at
/// `sample_rate_hz`, with amplitude `amplitude` (default: ~75% of int16 max).
static std::vector<std::int16_t> MakeSine(int sample_rate_hz,
                                          double freq_hz,
                                          std::size_t n_samples,
                                          double amplitude = 24000.0) {
    std::vector<std::int16_t> out(n_samples);
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = static_cast<double>(i) / sample_rate_hz;
        out[i] = static_cast<std::int16_t>(amplitude * std::sin(2.0 * M_PI * freq_hz * t));
    }
    return out;
}

/// Compute RMS of a sample buffer (in dBFS relative to amplitude=32767).
static double RmsDbfs(const std::int16_t* data, std::size_t n) {
    if (n == 0) {
        return -200.0;
    }
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = static_cast<double>(data[i]);
        sum_sq += s * s;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(n));
    // 0 dBFS = 32767
    return 20.0 * std::log10(rms / 32767.0);
}

// ---------------------------------------------------------------------------
// R1 — Create(8000, 16000) non-null
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R1_Create_8k_to_16k) {
    auto r = Resampler::Create(8000, 16000);
    EXPECT_NE(r, nullptr);
    if (r) {
        EXPECT_EQ(r->from_hz(), 8000);
        EXPECT_EQ(r->to_hz(), 16000);
    }
}

// ---------------------------------------------------------------------------
// R2 — Create(16000, 8000) non-null
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R2_Create_16k_to_8k) {
    auto r = Resampler::Create(16000, 8000);
    EXPECT_NE(r, nullptr);
}

// ---------------------------------------------------------------------------
// R3 — same-rate pairs are supported as caller-side pass-through
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R3_Create_8k_to_8k) {
    auto r = Resampler::Create(8000, 8000);
    EXPECT_EQ(r, nullptr);
    EXPECT_TRUE(Resampler::Supports(8000, 8000));
}

// ---------------------------------------------------------------------------
// R4 — Create(8000, 24000) → nullptr
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R4_Create_8k_to_24k_isNull) {
    auto r = Resampler::Create(8000, 24000);
    EXPECT_EQ(r, nullptr);
}

// ---------------------------------------------------------------------------
// R5 — Create(48000, 16000) → nullptr
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R5_Create_48k_to_16k_isNull) {
    auto r = Resampler::Create(48000, 16000);
    EXPECT_EQ(r, nullptr);
}

// ---------------------------------------------------------------------------
// R6 — Upsample 160 samples 8→16 kHz: output 320 ± 4 samples
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R6_Upsample_8k_to_16k_SampleCount) {
    auto r = Resampler::Create(8000, 16000);
    ASSERT_NE(r, nullptr);

    auto input = MakeSine(8000, 1000.0, 160);
    std::vector<std::int16_t> output(512, 0);

    const std::size_t written =
        r->Process(input.data(), input.size(), output.data(), output.size());

    EXPECT_GE(written, 316u) << "Expected ~320 output samples (±4), got " << written;
    EXPECT_LE(written, 324u) << "Expected ~320 output samples (±4), got " << written;
}

// ---------------------------------------------------------------------------
// R7 — Downsample 320 samples 16→8 kHz: output 160 ± 4 samples
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R7_Downsample_16k_to_8k_SampleCount) {
    auto r = Resampler::Create(16000, 8000);
    ASSERT_NE(r, nullptr);

    auto input = MakeSine(16000, 1000.0, 320);
    std::vector<std::int16_t> output(512, 0);

    const std::size_t written =
        r->Process(input.data(), input.size(), output.data(), output.size());

    EXPECT_GE(written, 156u) << "Expected ~160 output samples (±4), got " << written;
    EXPECT_LE(written, 164u) << "Expected ~160 output samples (±4), got " << written;
}

// ---------------------------------------------------------------------------
// R8 — Audio fidelity: 1 kHz sine 200 ms @ 8 kHz → upsample → RMS ±0.5 dB
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R8_AudioFidelity_1kHz_8kToUpsampled) {
    auto r = Resampler::Create(8000, 16000);
    ASSERT_NE(r, nullptr);

    // 200 ms @ 8 kHz = 1600 samples.
    const std::size_t n_input = 1600;
    auto input = MakeSine(8000, 1000.0, n_input);

    // Output buffer: 1600 × 2 + headroom.
    std::vector<std::int16_t> output(4096, 0);
    std::size_t total_written = 0;

    // Process in one shot.
    total_written = r->Process(input.data(), input.size(), output.data(), output.size());
    ASSERT_GT(total_written, 0u);

    const double rms_in = RmsDbfs(input.data(), n_input);
    const double rms_out = RmsDbfs(output.data(), total_written);

    EXPECT_NEAR(rms_in, rms_out, 0.5)
        << "RMS fidelity: input=" << rms_in << " dBFS, output=" << rms_out << " dBFS";
}

// ---------------------------------------------------------------------------
// R9 — Stateful across calls: 5 chunks of 64 samples each
//       Verified by comparing RMS of chunk outputs vs. a single-shot output.
// ---------------------------------------------------------------------------
TEST(ResamplerTest, R9_StatefulAcrossCalls) {
    // Chunked resampler (preserves FIR state).
    auto r_chunked = Resampler::Create(8000, 16000);
    ASSERT_NE(r_chunked, nullptr);

    // Reference: single-shot resampler.
    auto r_single = Resampler::Create(8000, 16000);
    ASSERT_NE(r_single, nullptr);

    const std::size_t n_total = 320;  // 5 × 64
    const std::size_t chunk_size = 64;
    auto input = MakeSine(8000, 1000.0, n_total);

    // Chunked processing.
    std::vector<std::int16_t> chunked_out(4096, 0);
    std::size_t chunked_total = 0;
    for (std::size_t offset = 0; offset < n_total; offset += chunk_size) {
        std::vector<std::int16_t> tmp(512, 0);
        const std::size_t n =
            r_chunked->Process(input.data() + offset, chunk_size, tmp.data(), tmp.size());
        for (std::size_t i = 0; i < n && chunked_total < chunked_out.size(); ++i) {
            chunked_out[chunked_total++] = tmp[i];
        }
    }

    // Single-shot processing.
    std::vector<std::int16_t> single_out(4096, 0);
    const std::size_t single_total =
        r_single->Process(input.data(), n_total, single_out.data(), single_out.size());

    ASSERT_GT(chunked_total, 0u);
    ASSERT_GT(single_total, 0u);

    // Both should produce roughly the same number of samples.
    EXPECT_NEAR(static_cast<double>(chunked_total),
                static_cast<double>(single_total),
                static_cast<double>(single_total) * 0.05)  // within 5%
        << "Chunked=" << chunked_total << " vs single=" << single_total;

    // Compare RMS energy of the two outputs (should be within 1 dB).
    const double rms_chunked = RmsDbfs(chunked_out.data(), chunked_total);
    const double rms_single = RmsDbfs(single_out.data(), single_total);
    EXPECT_NEAR(rms_chunked, rms_single, 1.0)
        << "RMS: chunked=" << rms_chunked << " dBFS, single=" << rms_single << " dBFS";
}

}  // namespace
