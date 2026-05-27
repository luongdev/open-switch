/*
 * src/media/purpose.cc — StageRank + PurposeName implementations.
 *
 * Stage rank table (mirrors designs/media-bridge.md §"Stage rank"):
 *
 *   EARLY   (1) — kVadBargeIn
 *   MID_READ(2) — kSttTranscribe, kVoicebotDuplexRead, kAmdDetect
 *   INJECT  (3) — kTtsPlayback, kVoicebotDuplexWrite
 *   LATE    (4) — kRecordingRelay, kTest, kUnspecified
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/purpose.h"

namespace osw::media {

std::uint32_t StageRank(Purpose p) noexcept {
    switch (p) {
        case Purpose::kVadBargeIn:
            return 1;  // EARLY
        case Purpose::kSttTranscribe:
        case Purpose::kVoicebotDuplexRead:
        case Purpose::kAmdDetect:
            return 2;  // MID_READ
        case Purpose::kTtsPlayback:
        case Purpose::kVoicebotDuplexWrite:
            return 3;  // INJECT
        case Purpose::kRecordingRelay:
        case Purpose::kTest:
        case Purpose::kUnspecified:
        default:
            return 4;  // LATE
    }
}

std::string_view PurposeName(Purpose p) noexcept {
    switch (p) {
        case Purpose::kUnspecified:
            return "unspecified";
        case Purpose::kTtsPlayback:
            return "tts_playback";
        case Purpose::kSttTranscribe:
            return "stt_transcribe";
        case Purpose::kVoicebotDuplexRead:
            return "voicebot_duplex_read";
        case Purpose::kVoicebotDuplexWrite:
            return "voicebot_duplex_write";
        case Purpose::kAmdDetect:
            return "amd_detect";
        case Purpose::kRecordingRelay:
            return "recording_relay";
        case Purpose::kVadBargeIn:
            return "vad_barge_in";
        case Purpose::kTest:
            return "test";
        default:
            return "unknown";
    }
}

}  // namespace osw::media
