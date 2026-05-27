/*
 * include/osw/control/var_denylist.h
 *
 * IsReservedVar — shared predicate for the reserved channel-variable
 * prefix denylist.
 *
 * Used by:
 *   - SetVariables handler (P1-5)
 *   - Execute handler app=set guard (P1-4)
 *   - Hangup handler variables guard (P2-9)
 *
 * The denylist covers variable name PREFIXES that gate FreeSWITCH hooks,
 * inject raw SIP headers, or trigger arbitrary code execution:
 *
 *   api_               — api_on_answer / api_on_hangup (arbitrary API call)
 *   exec_              — exec_on_* hooks
 *   bridge_pre_execute_
 *   bridge_post_bridge_
 *   hangup_after_bridge_
 *   sip_h_             — injects raw SIP headers into the INVITE/response
 *   record_            — record_post_process_exec etc.
 *   _record_
 *   wait_for_
 *   transfer_after_bridge
 *   session_in_hangup_hook   — arbitrary hangup hook injection
 *   api_on_            — catches api_on_answer / api_on_hangup
 *   execute_on_        — catches execute_on_answer / execute_on_hangup
 *
 * Comparison is case-insensitive: FreeSWITCH variable names are looked
 * up case-insensitively but stored as typed. We reject regardless of
 * the capitalisation the caller used.
 *
 * Cited FACTs: FF-026.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_VAR_DENYLIST_H_
#define OSW_CONTROL_VAR_DENYLIST_H_

#include <string>

namespace osw::control {

/// Returns true if `name` matches one of the reserved variable-name
/// prefixes (case-insensitive comparison). Callers MUST reject the
/// variable with INVALID_ARGUMENT without calling FS.
[[nodiscard]] bool IsReservedVar(const std::string& name) noexcept;

}  // namespace osw::control

#endif  // OSW_CONTROL_VAR_DENYLIST_H_
