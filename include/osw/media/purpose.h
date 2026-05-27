/*
 * include/osw/media/purpose.h
 *
 * Purpose enum for MediaBugManager bugs.  Each Purpose maps to a
 * stage rank used by the attach-order enforcement in MediaBugManager.
 * Free helpers allow tests to assert ordering without touching
 * MediaBugManager internals.
 *
 * Stage rank table (mirrors designs/media-bridge.md §"Stage rank"):
 *
 *   EARLY   (1) — kVadBargeIn          (always SMBF_FIRST)
 *   MID_READ(2) — kSttTranscribe, kVoicebotDuplexRead, kAmdDetect
 *   INJECT  (3) — kTtsPlayback, kVoicebotDuplexWrite
 *   LATE    (4) — kRecordingRelay (W7), kTest
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_PURPOSE_H_
#define OSW_MEDIA_PURPOSE_H_

#include <cstdint>
#include <string_view>

namespace osw::media {

enum class Purpose : std::uint8_t {
    kUnspecified = 0,
    kTtsPlayback,
    kSttTranscribe,
    kVoicebotDuplexRead,
    kVoicebotDuplexWrite,
    kAmdDetect,       // proto-reserved; not used by W6 (W7 or V2)
    kRecordingRelay,  // proto-reserved; W7
    kVadBargeIn,      // proto-reserved; V2
    kTest,            // internal smoke
};

/// Maps Purpose to numeric stage rank used by the order check.
/// Free helper so tests can assert ordering without poking BugManager
/// internals.
[[nodiscard]] std::uint32_t StageRank(Purpose p) noexcept;

/// String form for log + audit.  Matches snake-case Purpose enum names
/// in proto/open_switch/media/v1/media.proto.
[[nodiscard]] std::string_view PurposeName(Purpose p) noexcept;

}  // namespace osw::media

#endif  // OSW_MEDIA_PURPOSE_H_
