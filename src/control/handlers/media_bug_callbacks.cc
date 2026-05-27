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
#include "osw/raii/fs_api.h"

#include <cstring>

#include "osw/media/audio_frame.h"
#include "osw/media/bug_manager.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"
#include "osw/raii/fs_api.h"

#include "src/control/handlers/media_bug_callbacks.h"

// ---------------------------------------------------------------------------
// ABC type constants (not defined by fs_mock.h; define here for both paths).
// ---------------------------------------------------------------------------

namespace {

#if defined(OSW_TEST_FS_MOCK)
constexpr int kAbcTypeRead = 1;        // SWITCH_ABC_TYPE_READ
constexpr int kAbcTypeReadPing = 5;    // SWITCH_ABC_TYPE_READ_PING
constexpr int kAbcTypeWriteReplace = 3; // SWITCH_ABC_TYPE_WRITE_REPLACE
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

// Helper: compute next sequence number. We use a per-bug monotonic counter
// stored in a field we add to WriteCallbackCtx for STT, or the AudioFrame
// seq field directly. For read tap, we track seq on the client side via a
// simple atomic in the callback context.
static std::uint64_t IncrementSeq(std::atomic<std::uint64_t>& seq) noexcept {
    return seq.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// ---------------------------------------------------------------------------
// Seq-number state for read-tap callbacks.  One instance per stream;
// allocated/freed alongside WriteCallbackCtx / StreamClient* pointer.
// ---------------------------------------------------------------------------
struct OswReadTapCtx {
    osw::media::StreamClient* client;
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> ts_samples{0};
};

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
    const std::uint32_t ch = (frame->channels > 0) ? static_cast<std::uint32_t>(frame->channels) : 1;

    std::vector<std::int16_t> samples(src, src + n_samples);

    static thread_local std::uint64_t tl_seq = 0;
    static thread_local std::uint64_t tl_ts = 0;
    const std::uint64_t seq = tl_seq++;
    const std::uint64_t ts = tl_ts;
    tl_ts += n_samples / ch;

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
    frame->samples = written;
    frame->datalen = written * sizeof(std::int16_t);
    osw::raii::fs::MediaBugSetWriteReplaceFrame(bug, frame);
#else
    // In tests, switch_frame_t is opaque. Tests drive the buffer directly.
    (void)frame;
#endif

    return SWITCH_TRUE;
}
