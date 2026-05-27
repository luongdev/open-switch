/*
 * src/control/handlers/media_bug_callbacks.h
 *
 * File-static bug callbacks for media streaming (W6 Track C).
 *
 * Per FF-003 these are extern "C" functions registered as the user_cb in
 * BugCallbackContext. BugCallbackContext::user_data points at:
 *   - For OswStreamingReadTap: a StreamClient* (non-owning)
 *   - For OswStreamingWriteReplace: a WriteCallbackCtx* (non-owning)
 *
 * Private to src/control/. Not exported.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_MEDIA_BUG_CALLBACKS_H_
#define OSW_CONTROL_HANDLERS_MEDIA_BUG_CALLBACKS_H_

// Include FS headers or mock seam before any FS-type-using declarations.
// Uses the canonical fs_api.h seam (same as all other FS-dependent handlers).
#include "osw/raii/fs_api.h"

#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"

namespace osw::control::handlers {

/// Context passed as BugCallbackContext::user_data for the write-replace
/// callback. Owns nothing — both pointers are owned by ActiveMediaStream.
struct WriteCallbackCtx {
    osw::media::StreamClient* client;
    osw::media::TtsPlayoutBuffer* buffer;
};

}  // namespace osw::control::handlers

extern "C" {

/// Read-tap bug callback (STT / voicebot read side).
/// Copies the read frame's samples into an AudioFrame and calls
/// client->SendAudio() without blocking.
/// user_data is a StreamClient* (non-owning).
switch_bool_t OswStreamingReadTap(switch_media_bug_t* bug,
                                  void* user_data,
                                  switch_abc_type_t type) noexcept;

/// Write-replace bug callback (TTS / voicebot write side).
/// Pops the next AudioFrame from TtsPlayoutBuffer and copies it into
/// the FS write-replace frame. user_data is a WriteCallbackCtx*.
switch_bool_t OswStreamingWriteReplace(switch_media_bug_t* bug,
                                       void* user_data,
                                       switch_abc_type_t type) noexcept;

}  // extern "C"

#endif  // OSW_CONTROL_HANDLERS_MEDIA_BUG_CALLBACKS_H_
