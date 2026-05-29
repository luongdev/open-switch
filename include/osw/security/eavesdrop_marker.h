/*
 * include/osw/security/eavesdrop_marker.h
 *
 * Channel variable marker used by W7 Track A eavesdrop enforcement.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_SECURITY_EAVESDROP_MARKER_H_
#define OSW_SECURITY_EAVESDROP_MARKER_H_

#include <string_view>

#include "osw/raii/fs_api.h"
#include "osw/security/eavesdrop_policy.h"

namespace osw::security {

inline constexpr const char* kBotSessionVar = "osw_bot_session";
inline constexpr const char* kBotPurposeVar = "osw_bot_purpose";
inline constexpr const char* kEavesdropPolicyVar = "osw_eavesdrop_policy";
inline constexpr const char* kTenantVar = "osw_tenant";

/// Sets osw_bot_session=true, osw_bot_purpose, osw_eavesdrop_policy,
/// and osw_tenant on a locked target session. Idempotent and best
/// effort; null sessions or channels are no-ops.
void MarkBotSession(switch_core_session_t* session,
                    std::string_view purpose,
                    EavesdropPolicy policy,
                    std::string_view tenant_id) noexcept;

}  // namespace osw::security

#endif  // OSW_SECURITY_EAVESDROP_MARKER_H_
