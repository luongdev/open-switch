/*
 * include/osw/security/eavesdrop_detector.h
 *
 * W7 Track A MEDIA_BUG_START detector for raw eavesdrop backstop audit.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_SECURITY_EAVESDROP_DETECTOR_H_
#define OSW_SECURITY_EAVESDROP_DETECTOR_H_

#include "osw/raii/fs_api.h"

namespace osw::security {

bool BindEavesdropDetector() noexcept;
void UnbindEavesdropDetector() noexcept;

/// Exposed for FS-mock unit tests. The detector never removes an FS-native
/// eavesdrop bug; when policy resolves to deny it fails closed by hanging up
/// the target bot channel after emitting the post-attach audit.
void HandleMediaBugStartForTest(switch_event_t* event) noexcept;

}  // namespace osw::security

#endif  // OSW_SECURITY_EAVESDROP_DETECTOR_H_
