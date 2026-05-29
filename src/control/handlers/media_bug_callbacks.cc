/*
 * src/control/handlers/media_bug_callbacks.cc
 *
 * Implementation of OswStreamingReadTap + OswStreamingWriteReplace.
 *
 * Threading:
 *   - OswStreamingReadTap runs on the FS media thread for the channel.
 *   - OswStreamingWriteReplace runs on the same FS media thread.
 *   Both must return quickly. No blocking.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Include FS headers or mock seam first via the canonical seam.
#include "src/control/handlers/media_bug_callbacks.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "osw/media/audio_frame.h"
#include "osw/media/bot_session.h"
#include "osw/media/bug_manager.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

// ---------------------------------------------------------------------------
// ABC type constants (not defined by fs_mock.h; define here for both paths).
// ---------------------------------------------------------------------------

namespace {

#if defined(OSW_TEST_FS_MOCK)
constexpr int kAbcTypeRead = 1;          // SWITCH_ABC_TYPE_READ
constexpr int kAbcTypeReadPing = 5;      // SWITCH_ABC_TYPE_READ_PING
constexpr int kAbcTypeWriteReplace = 3;  // SWITCH_ABC_TYPE_WRITE_REPLACE
constexpr int kAbcTypeInit = 0;
constexpr int kAbcTypeClose = 8;
#else
constexpr const char* kWriteReplaceSubsystem = "media.write_replace";
constexpr int kAbcTypeRead = static_cast<int>(SWITCH_ABC_TYPE_READ);
constexpr int kAbcTypeReadPing = static_cast<int>(SWITCH_ABC_TYPE_READ_PING);
constexpr int kAbcTypeWriteReplace = static_cast<int>(SWITCH_ABC_TYPE_WRITE_REPLACE);
constexpr int kAbcTypeInit = static_cast<int>(SWITCH_ABC_TYPE_INIT);
constexpr int kAbcTypeClose = static_cast<int>(SWITCH_ABC_TYPE_CLOSE);
#endif

constexpr const char* kBotCallbackSubsystem = "media.bot_callback";
constexpr const char* kStreamingCallbackSubsystem = "media.streaming_callback";

// The BugCallbackContext is defined in bug_manager.cc. We access it via the
// type defined there — it is deliberately public-but-internal in that TU.
// We forward-declare the struct matching what bug_manager.cc defines.
struct BugCallbackContextFwd {
    osw::media::MediaBugManager* manager;
    std::uint64_t bug_id;
    void* user_data;
    switch_media_bug_callback_t user_cb;
};

[[maybe_unused]] std::size_t ResampledCapacity(std::size_t in_samples,
                                               std::uint32_t from_hz,
                                               std::uint32_t to_hz) noexcept {
    if (in_samples == 0 || from_hz == 0 || to_hz == 0) {
        return 0;
    }
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(in_samples) * static_cast<std::uint64_t>(to_hz);
    return static_cast<std::size_t>((numerator + from_hz - 1u) / from_hz) + 16u;
}

[[maybe_unused]] bool EnsureResampler(std::unique_ptr<osw::media::Resampler>* resampler,
                                      std::uint32_t from_hz,
                                      std::uint32_t to_hz,
                                      bool* error_logged,
                                      const char* subsystem,
                                      const char* stream_id) noexcept {
    if (from_hz == to_hz) {
        return true;
    }
    if (!resampler) {
        return false;
    }
    if (!*resampler) {
        *resampler =
            osw::media::Resampler::Create(static_cast<int>(from_hz), static_cast<int>(to_hz));
    }
    if (*resampler) {
        return true;
    }
    if (error_logged && !*error_logged) {
        *error_logged = true;
        osw::log::Error(subsystem,
                        "unsupported media rate pair stream_id=%s from_hz=%u to_hz=%u",
                        stream_id ? stream_id : "",
                        from_hz,
                        to_hz);
    }
    return false;
}

}  // namespace

// W6.5 P4-001 fix: removed dead OswReadTapCtx struct.  The seq + ts
// counters now live on StreamClient (Gemini-P1 fix replaces the
// `static thread_local` state below that leaked across pooled FS
// media threads).
//
// W6.5 P2-003 follow-up: a future refactor may upgrade user_data to a
// proper per-stream context that ALSO carries a Resampler instance
// (per FF-033), but the seq/ts portion is already fixed by routing
// through StreamClient::NextSeq + AdvanceTimestamp.

// OswStreamingReadTap --------------------------------------------------------

extern "C" switch_bool_t OswStreamingReadTap(switch_media_bug_t* bug,
                                             void* user_data,
                                             switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<osw::control::handlers::ReadCallbackCtx*>(user_data);
    try {
        const int t = static_cast<int>(type);
        if (t == kAbcTypeInit || t == kAbcTypeClose) {
            return SWITCH_TRUE;
        }
        if (t != kAbcTypeRead && t != kAbcTypeReadPing) {
            return SWITCH_TRUE;
        }

        if (!ctx || !ctx->client) {
            return SWITCH_TRUE;
        }

#if !defined(OSW_TEST_FS_MOCK)
        // SMBF_READ_STREAM callbacks must pull the current frame with
        // switch_core_media_bug_read. Read-replace accessors are for
        // READ_REPLACE-style bugs and do not carry plain read-stream audio.
        switch_frame_t frame{};
        if (osw::raii::fs::MediaBugRead(bug, &frame, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
            frame.datalen == 0 || !frame.data) {
            return SWITCH_TRUE;
        }

        const auto* src = static_cast<const std::int16_t*>(frame.data);
        const std::uint32_t n_samples = frame.datalen / sizeof(std::int16_t);
        const std::uint32_t in_rate = frame.rate != 0 ? frame.rate : ctx->fs_rate_hz;
        const std::uint32_t out_rate = ctx->stream_rate_hz != 0 ? ctx->stream_rate_hz : in_rate;
        const std::uint32_t ch =
            (frame.channels > 0) ? static_cast<std::uint32_t>(frame.channels) : 1;

        std::vector<std::int16_t> samples;
        if (in_rate != out_rate) {
            if (ch != 1 || !EnsureResampler(&ctx->resampler,
                                            in_rate,
                                            out_rate,
                                            &ctx->resampler_error_logged,
                                            kStreamingCallbackSubsystem,
                                            "")) {
                return SWITCH_TRUE;
            }
            samples.resize(ResampledCapacity(n_samples, in_rate, out_rate));
            const std::size_t written =
                ctx->resampler->Process(src, n_samples, samples.data(), samples.size());
            if (written == 0) {
                return SWITCH_TRUE;
            }
            samples.resize(written);
        } else {
            samples.assign(src, src + n_samples);
        }

        // W6.5 Gemini-P1 fix: per-stream seq + ts via StreamClient atomics.
        const std::uint64_t seq = ctx->client->NextSeq();
        const std::uint64_t ts =
            ctx->client->AdvanceTimestamp(static_cast<std::uint32_t>(samples.size() / ch));

        osw::media::AudioFrame af(std::move(samples), out_rate, ch, seq, ts);
        ctx->client->SendAudio(std::move(af));
#else
        // In tests, switch_frame_t is opaque (incomplete type). The test injects
        // frames through the buffer->Push path directly; the callback is invoked
        // as a no-op in mock mode (tests drive the buffer directly).
        (void)bug;
#endif

        return SWITCH_TRUE;
    } catch (...) {
        osw::log::Error(kStreamingCallbackSubsystem, "OswStreamingReadTap exception");
    }
    return SWITCH_TRUE;
}

// OswStreamingWriteReplace ---------------------------------------------------

extern "C" switch_bool_t OswStreamingWriteReplace(switch_media_bug_t* bug,
                                                  void* user_data,
                                                  switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<osw::control::handlers::WriteCallbackCtx*>(user_data);
    try {
        const int t = static_cast<int>(type);
        if (t == kAbcTypeInit || t == kAbcTypeClose) {
            return SWITCH_TRUE;
        }
        if (t != kAbcTypeWriteReplace) {
            return SWITCH_TRUE;
        }

        if (!ctx || !ctx->buffer) {
            return SWITCH_TRUE;
        }

        auto* frame = osw::raii::fs::MediaBugGetWriteReplaceFrame(bug);
        if (!frame) {
            return SWITCH_TRUE;
        }

#if !defined(OSW_TEST_FS_MOCK)
        // Pop samples from the jitter buffer into the FS frame. When the
        // upstream stream rate differs from the FS write rate, pop the
        // equivalent duration at stream rate and resample once per callback.
        auto* dst = static_cast<std::int16_t*>(frame->data);
        const std::uint32_t cap_samples = frame->datalen / sizeof(std::int16_t);
        const std::uint32_t fs_rate = frame->rate != 0 ? frame->rate : ctx->fs_rate_hz;
        const std::uint32_t stream_rate = ctx->stream_rate_hz != 0 ? ctx->stream_rate_hz : fs_rate;

        std::uint32_t written = 0;
        if (fs_rate != stream_rate) {
            if (!EnsureResampler(&ctx->resampler,
                                 stream_rate,
                                 fs_rate,
                                 &ctx->resampler_error_logged,
                                 kStreamingCallbackSubsystem,
                                 ctx->stream_id.c_str())) {
                return SWITCH_TRUE;
            }
            const std::uint32_t needed_at_stream_rate =
                static_cast<std::uint32_t>(std::max<std::size_t>(
                    1u, ResampledCapacity(cap_samples, fs_rate, stream_rate) - 16u));
            std::vector<std::int16_t> tmp(needed_at_stream_rate);
            const std::uint32_t popped = ctx->buffer->Pop(tmp.data(), needed_at_stream_rate);
            if (popped == 0) {
                if (ctx->buffer->EndOfStream()) {
                    return SWITCH_FALSE;
                }
                return SWITCH_TRUE;
            }
            written = static_cast<std::uint32_t>(
                ctx->resampler->Process(tmp.data(), popped, dst, cap_samples));
            if (written == 0) {
                return SWITCH_TRUE;
            }
        } else {
            written = ctx->buffer->Pop(dst, cap_samples);
            if (written == 0) {
                if (ctx->buffer->EndOfStream()) {
                    return SWITCH_FALSE;
                }
                return SWITCH_TRUE;
            }
        }

        frame->samples = written;
        frame->datalen = written * sizeof(std::int16_t);
        frame->rate = fs_rate;
        osw::raii::fs::MediaBugSetWriteReplaceFrame(bug, frame);
        if (!ctx->first_set_frame_logged) {
            ctx->first_set_frame_logged = true;
            osw::log::Info(kWriteReplaceSubsystem,
                           "event=osw.write_replace.first_set_frame stream_id=%s samples=%u "
                           "payload_bytes=%u",
                           ctx->stream_id.c_str(),
                           static_cast<unsigned>(written),
                           static_cast<unsigned>(frame->datalen));
        }
#else
        // In tests, switch_frame_t is opaque. Tests drive the buffer directly.
        (void)frame;
#endif

        return SWITCH_TRUE;
    } catch (...) {
        osw::log::Error(kStreamingCallbackSubsystem,
                        "OswStreamingWriteReplace exception stream_id=%s",
                        ctx ? ctx->stream_id.c_str() : "?");
    }
    return SWITCH_TRUE;
}

extern "C" switch_bool_t OswBotReadTap(switch_media_bug_t* bug,
                                       void* user_data,
                                       switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<osw::media::BotReadTapCtx*>(user_data);
    try {
        const int t = static_cast<int>(type);

        if (t == kAbcTypeClose) {
            if (ctx && ctx->bot) {
                ctx->bot->OnTargetClose(ctx->channel_uuid, /*direction=*/0);
            }
            return SWITCH_TRUE;
        }
        if (t == kAbcTypeInit || (t != kAbcTypeRead && t != kAbcTypeReadPing)) {
            return SWITCH_TRUE;
        }
        if (!ctx || !ctx->bot || ctx->bot->IsStopped()) {
            return SWITCH_TRUE;
        }

#if !defined(OSW_TEST_FS_MOCK)
        switch_frame_t frame{};
        if (osw::raii::fs::MediaBugRead(bug, &frame, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
            frame.datalen == 0 || !frame.data) {
            return SWITCH_TRUE;
        }
        const auto* src = static_cast<const std::int16_t*>(frame.data);
        const std::uint32_t n_samples = frame.datalen / sizeof(std::int16_t);
        const std::uint32_t in_rate = frame.rate != 0 ? frame.rate : ctx->fs_rate_hz;
        const std::uint32_t out_rate = ctx->stream_rate_hz != 0 ? ctx->stream_rate_hz : in_rate;
        const std::uint32_t channels =
            (frame.channels > 0) ? static_cast<std::uint32_t>(frame.channels) : 1;
        if (in_rate != out_rate) {
            if (channels != 1 || !EnsureResampler(&ctx->resampler,
                                                  in_rate,
                                                  out_rate,
                                                  &ctx->resampler_error_logged,
                                                  kBotCallbackSubsystem,
                                                  ctx->channel_uuid.c_str())) {
                return SWITCH_TRUE;
            }
            std::vector<std::int16_t> converted(ResampledCapacity(n_samples, in_rate, out_rate));
            const std::size_t written =
                ctx->resampler->Process(src, n_samples, converted.data(), converted.size());
            if (written == 0) {
                return SWITCH_TRUE;
            }
            converted.resize(written);
            ctx->bot->OnTargetReadFrame(ctx->channel_uuid,
                                        frame.timestamp,
                                        converted.data(),
                                        converted.size(),
                                        out_rate,
                                        channels);
        } else {
            ctx->bot->OnTargetReadFrame(
                ctx->channel_uuid, frame.timestamp, src, n_samples, out_rate, channels);
        }
#else
        (void)bug;
#endif

        return SWITCH_TRUE;
    } catch (...) {
        osw::log::Error(kBotCallbackSubsystem,
                        "OswBotReadTap exception channel=%s",
                        ctx ? ctx->channel_uuid.c_str() : "?");
    }
    return SWITCH_TRUE;
}

extern "C" switch_bool_t OswBotWriteReplace(switch_media_bug_t* bug,
                                            void* user_data,
                                            switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<osw::media::BotWriteReplaceCtx*>(user_data);
    try {
        const int t = static_cast<int>(type);

        if (t == kAbcTypeClose) {
            if (ctx && ctx->bot) {
                ctx->bot->OnTargetClose(ctx->channel_uuid, /*direction=*/1);
            }
            return SWITCH_TRUE;
        }
        if (t == kAbcTypeInit || t != kAbcTypeWriteReplace) {
            return SWITCH_TRUE;
        }
        if (!ctx || !ctx->bot || ctx->bot->IsStopped()) {
            return SWITCH_TRUE;
        }

        auto frame_opt = ctx->bot->PopWriteFrame(ctx->channel_uuid);
        if (!frame_opt.has_value()) {
            // Passthrough when bot is silent: leave FS's write_replace_frame_out
            // untouched by intentionally not calling MediaBugSetWriteReplaceFrame.
            return SWITCH_TRUE;
        }

#if !defined(OSW_TEST_FS_MOCK)
        auto* frame = osw::raii::fs::MediaBugGetWriteReplaceFrame(bug);
        if (!frame || !frame->data || frame->datalen == 0) {
            return SWITCH_TRUE;
        }

        auto* dst = static_cast<std::int16_t*>(frame->data);
        const std::uint32_t cap_samples = frame->datalen / sizeof(std::int16_t);
        const std::uint32_t fs_rate = frame->rate != 0 ? frame->rate : ctx->fs_rate_hz;
        const std::uint32_t stream_rate = frame_opt->sample_rate_hz();
        std::uint32_t copy_samples = 0;
        if (fs_rate != 0 && stream_rate != 0 && fs_rate != stream_rate) {
            if (!EnsureResampler(&ctx->resampler,
                                 stream_rate,
                                 fs_rate,
                                 &ctx->resampler_error_logged,
                                 kBotCallbackSubsystem,
                                 ctx->channel_uuid.c_str())) {
                return SWITCH_TRUE;
            }
            copy_samples = static_cast<std::uint32_t>(ctx->resampler->Process(
                frame_opt->data(), frame_opt->sample_count(), dst, cap_samples));
        } else {
            copy_samples = std::min<std::uint32_t>(
                cap_samples, static_cast<std::uint32_t>(frame_opt->sample_count()));
            std::memcpy(dst, frame_opt->data(), copy_samples * sizeof(std::int16_t));
        }
        if (copy_samples == 0) {
            return SWITCH_TRUE;
        }
        frame->samples = copy_samples;
        frame->datalen = copy_samples * sizeof(std::int16_t);
        frame->rate = fs_rate != 0 ? fs_rate : stream_rate;
        osw::raii::fs::MediaBugSetWriteReplaceFrame(bug, frame);

        if (!ctx->first_set_frame_logged) {
            ctx->first_set_frame_logged = true;
            osw::log::Info(kWriteReplaceSubsystem,
                           "event=osw.bot.write_replace.first_set_frame bot_target=%s samples=%u "
                           "payload_bytes=%u",
                           ctx->channel_uuid.c_str(),
                           static_cast<unsigned>(copy_samples),
                           static_cast<unsigned>(frame->datalen));
        }
#else
        osw::raii::fs::MediaBugSetWriteReplaceFrame(bug, nullptr);
#endif

        return SWITCH_TRUE;
    } catch (...) {
        osw::log::Error(kBotCallbackSubsystem,
                        "OswBotWriteReplace exception channel=%s",
                        ctx ? ctx->channel_uuid.c_str() : "?");
    }
    return SWITCH_TRUE;
}
