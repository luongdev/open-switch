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

#include <cstring>

#include "osw/media/audio_frame.h"
#include "osw/media/bug_manager.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

// ---------------------------------------------------------------------------
// ABC type constants (not defined by fs_mock.h; define here for both paths).
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kWriteReplaceSubsystem = "media.write_replace";

#if defined(OSW_TEST_FS_MOCK)
constexpr int kAbcTypeRead = 1;          // SWITCH_ABC_TYPE_READ
constexpr int kAbcTypeReadPing = 5;      // SWITCH_ABC_TYPE_READ_PING
constexpr int kAbcTypeWriteReplace = 3;  // SWITCH_ABC_TYPE_WRITE_REPLACE
constexpr int kAbcTypeInit = 0;
constexpr int kAbcTypeClose = 8;
#else
constexpr int kAbcTypeRead = static_cast<int>(SWITCH_ABC_TYPE_READ);
constexpr int kAbcTypeReadPing = static_cast<int>(SWITCH_ABC_TYPE_READ_PING);
constexpr int kAbcTypeWriteReplace = static_cast<int>(SWITCH_ABC_TYPE_WRITE_REPLACE);
constexpr int kAbcTypeInit = static_cast<int>(SWITCH_ABC_TYPE_INIT);
constexpr int kAbcTypeClose = static_cast<int>(SWITCH_ABC_TYPE_CLOSE);
#endif

// The BugCallbackContext is defined in bug_manager.cc. We access it via the
// type defined there — it is deliberately public-but-internal in that TU.
// We forward-declare the struct matching what bug_manager.cc defines.
struct BugCallbackContextFwd {
    osw::media::MediaBugManager* manager;
    std::uint64_t bug_id;
    void* user_data;
    switch_media_bug_callback_t user_cb;
};

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
    const int t = static_cast<int>(type);
    if (t == kAbcTypeInit || t == kAbcTypeClose) {
        return SWITCH_TRUE;
    }
    if (t != kAbcTypeRead && t != kAbcTypeReadPing) {
        return SWITCH_TRUE;
    }

    auto* client = static_cast<osw::media::StreamClient*>(user_data);
    if (!client) {
        return SWITCH_TRUE;
    }

    // Get the read-replace frame (read tap uses SMBF_READ_STREAM, which
    // uses switch_core_media_bug_get_read_replace_frame in WRITE_REPLACE
    // mode — but for READ_STREAM we use the read frame directly).
    // For SMBF_READ_STREAM, the correct API is get_read_replace_frame
    // (used in read-stream callbacks when SMBF_READ_REPLACE is not set
    // the frame is still available via this call with a copy).
    auto* frame = osw::raii::fs::MediaBugGetReadReplaceFrame(bug);
    if (!frame) {
        return SWITCH_TRUE;
    }

#if !defined(OSW_TEST_FS_MOCK)
    // In production, switch_frame_t has data / datalen / samples / rate fields.
    if (frame->datalen == 0 || !frame->data) {
        return SWITCH_TRUE;
    }

    const auto* src = static_cast<const std::int16_t*>(frame->data);
    const std::uint32_t n_samples = frame->datalen / sizeof(std::int16_t);
    const std::uint32_t rate = frame->rate;
    const std::uint32_t ch =
        (frame->channels > 0) ? static_cast<std::uint32_t>(frame->channels) : 1;

    std::vector<std::int16_t> samples(src, src + n_samples);

    // W6.5 Gemini-P1 fix: per-stream seq + ts via StreamClient atomics.
    // Previously `static thread_local` globals were used; FS pools its
    // media threads and reuses one thread across many channels, so the
    // seq/ts values leaked across unrelated calls — sequences would
    // skip, timestamps from a previous call would bleed into the next.
    // StreamClient owns one of each per stream, so values are isolated.
    const std::uint64_t seq = client->NextSeq();
    const std::uint64_t ts = client->AdvanceTimestamp(n_samples / ch);

    osw::media::AudioFrame af(std::move(samples), rate, ch, seq, ts);
    client->SendAudio(std::move(af));
#else
    // In tests, switch_frame_t is opaque (incomplete type). The test injects
    // frames through the buffer->Push path directly; the callback is invoked
    // as a no-op in mock mode (tests drive the buffer directly).
    (void)frame;
#endif

    return SWITCH_TRUE;
}

// OswStreamingWriteReplace ---------------------------------------------------

extern "C" switch_bool_t OswStreamingWriteReplace(switch_media_bug_t* bug,
                                                  void* user_data,
                                                  switch_abc_type_t type) noexcept {
    const int t = static_cast<int>(type);
    if (t == kAbcTypeInit || t == kAbcTypeClose) {
        return SWITCH_TRUE;
    }
    if (t != kAbcTypeWriteReplace) {
        return SWITCH_TRUE;
    }

    auto* ctx = static_cast<osw::control::handlers::WriteCallbackCtx*>(user_data);
    if (!ctx || !ctx->buffer) {
        return SWITCH_TRUE;
    }

    auto* frame = osw::raii::fs::MediaBugGetWriteReplaceFrame(bug);
    if (!frame) {
        return SWITCH_TRUE;
    }

#if !defined(OSW_TEST_FS_MOCK)
    // Pop samples from the jitter buffer into the FS frame.
    auto* dst = static_cast<std::int16_t*>(frame->data);
    const std::uint32_t cap_samples = frame->datalen / sizeof(std::int16_t);

    const std::uint32_t written = ctx->buffer->Pop(dst, cap_samples);
    if (written == 0) {
        if (ctx->buffer->EndOfStream()) {
            return SWITCH_FALSE;
        }
        return SWITCH_TRUE;
    }

    frame->samples = written;
    frame->datalen = written * sizeof(std::int16_t);
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
}
