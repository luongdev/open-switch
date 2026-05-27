/*
 * src/media/resampler.cc
 *
 * Implementation of osw::media::Resampler.
 *
 * Wraps switch_audio_resampler_t (FreeSWITCH built-in spandsp wrapper).
 * Key findings from switch_resample.h in base image (FF-033):
 *   - Type:    switch_audio_resampler_t (not switch_resample_t)
 *   - Create:  switch_resample_create(&res, from_hz, to_hz, to_size,
 *                SWITCH_RESAMPLE_QUALITY, channels)
 *              Returns SWITCH_STATUS_SUCCESS on success.
 *   - Process: switch_resample_process(res, src, srclen)
 *              Output lands in res->to (int16_t*), length in res->to_len.
 *              Note: switch_resample_process takes a non-const int16_t* src.
 *   - Destroy: switch_resample_destroy(&res)  — nulls the pointer.
 *
 * The FS resampler preserves FIR filter state across calls.
 * DO NOT destroy and re-create between frames — see FF-033.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/resampler.h"

#include <cstring>

#include "osw/observability/log.h"

// FreeSWITCH master header — must be included directly (not switch_resample.h)
// because switch_resample.h → switch.h → switch_module_interfaces.h in a
// circular chain that references switch_audio_resampler_t before it is defined.
// Including switch.h directly respects the canonical include order.
//
// The include path set by CMake is /usr/local/include (the FS root, not a
// freeswitch/ subdirectory) so the bare <switch.h> form works.
// NOLINTNEXTLINE(build/include_order)
#include <switch.h>  // provides switch_audio_resampler_t + switch_resample_* API

namespace osw::media {

namespace {

// V1 policy: only these pairs are supported.
bool IsSupportedPair(int from_hz, int to_hz) noexcept {
    if (from_hz == to_hz) {
        return from_hz == 8000 || from_hz == 16000;
    }
    return (from_hz == 8000 && to_hz == 16000) || (from_hz == 16000 && to_hz == 8000);
}

}  // namespace

// ---------------------------------------------------------------------------
// static Create
// ---------------------------------------------------------------------------

std::unique_ptr<Resampler> Resampler::Create(int from_hz, int to_hz) noexcept {
    if (!IsSupportedPair(from_hz, to_hz)) {
        osw::log::Warn("media",
                       "Resampler::Create: unsupported pair %d → %d (V1 supports 8k↔16k only)",
                       from_hz,
                       to_hz);
        return nullptr;
    }

    // Calculate an appropriate output buffer size.
    // For 20ms ptime the largest input is 320 samples (16kHz mono).
    // Upsampling 8→16: 160 in → 320 out; downsample 16→8: 320 in → 160 out.
    // We allocate conservatively: max_input × ratio + a small headroom.
    // The FS macro switch_resample_calc_buffer_size does: (to/from) * srclen * 2
    // We use 1024 samples as the to_size (plenty for any V1 frame).
    const std::uint32_t to_size = 1024;

    switch_audio_resampler_t* res = nullptr;
    const switch_status_t status =
        switch_resample_create(&res,
                               static_cast<std::uint32_t>(from_hz),
                               static_cast<std::uint32_t>(to_hz),
                               to_size,
                               SWITCH_RESAMPLE_QUALITY,
                               1 /* channels — caller resamples each channel separately */);

    if (status != SWITCH_STATUS_SUCCESS || res == nullptr) {
        osw::log::Error("media",
                        "Resampler::Create: switch_resample_create failed (status=%d) "
                        "from_hz=%d to_hz=%d",
                        static_cast<int>(status),
                        from_hz,
                        to_hz);
        return nullptr;
    }

    return std::unique_ptr<Resampler>(new Resampler(static_cast<void*>(res), from_hz, to_hz));
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Resampler::Resampler(void* res, int from_hz, int to_hz) noexcept
    : resampler_(res), from_hz_(from_hz), to_hz_(to_hz) {}

Resampler::~Resampler() noexcept {
    if (resampler_ != nullptr) {
        auto* res = static_cast<switch_audio_resampler_t*>(resampler_);
        switch_resample_destroy(&res);
        resampler_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

std::size_t Resampler::Process(const std::int16_t* in,
                               std::size_t in_samples,
                               std::int16_t* out,
                               std::size_t out_cap) noexcept {
    if (resampler_ == nullptr || in == nullptr || out == nullptr || in_samples == 0) {
        return 0;
    }

    auto* res = static_cast<switch_audio_resampler_t*>(resampler_);

    // switch_resample_process takes a non-const int16_t* (the FS API has not
    // been updated to const-correct). We cast away const here; the function
    // only reads the input buffer and the cast is safe.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    switch_resample_process(
        res, const_cast<std::int16_t*>(in), static_cast<std::uint32_t>(in_samples));

    const std::size_t written = static_cast<std::size_t>(res->to_len);
    if (written == 0) {
        // Rare FS-internal error (e.g. underrun on first call).
        return 0;
    }

    if (out_cap < written) {
        osw::log::Warn("media",
                       "Resampler::Process: out_cap (%zu) < to_len (%zu); dropping frame",
                       out_cap,
                       written);
        return 0;
    }

    std::memcpy(out, res->to, written * sizeof(std::int16_t));
    return written;
}

}  // namespace osw::media
