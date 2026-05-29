/*
 * include/osw/security/eavesdrop_audit.h
 *
 * Shared audit emission for W7 Track A eavesdrop enforcement.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_SECURITY_EAVESDROP_AUDIT_H_
#define OSW_SECURITY_EAVESDROP_AUDIT_H_

#include <string_view>

#include "osw/raii/fs_api.h"
#include "osw/security/eavesdrop_policy.h"

namespace osw::security {

void EmitEavesdropAudit(switch_core_session_t* supervisor_session,
                        switch_core_session_t* target_session,
                        EavesdropPolicy policy,
                        std::string_view layer,
                        std::string_view decision) noexcept;

}  // namespace osw::security

#endif  // OSW_SECURITY_EAVESDROP_AUDIT_H_
