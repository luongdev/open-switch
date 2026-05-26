/*
 * include/osw/control/call_cause.h
 *
 * osw::control::CallCause — two-way mapping between the FreeSWITCH
 * switch_call_cause_t enum and the proto cause string used by the
 * control-plane RPCs (HangupRequest.cause, HangupManyRequest.cause,
 * OriginateResponse error detail).
 *
 * The proto represents causes as strings (e.g. "NORMAL_CLEARING",
 * "USER_BUSY") matching the names produced by FS's own
 * switch_channel_cause2str() / switch_channel_str2cause() (see
 * FREESWITCH-FACTS FF-022). This helper is a pure-function, stateless
 * mapping layer — no allocation, no global state.
 *
 * Null / unrecognised FS values → "UNSPECIFIED" (string).
 * Null / empty / unrecognised proto string → SWITCH_CAUSE_NORMAL_CLEARING
 * (the FS-side graceful-hangup default).
 *
 * Cited FACTs:
 *   - FF-022 — switch_channel_hangup / switch_channel_cause semantics.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_CALL_CAUSE_H_
#define OSW_CONTROL_CALL_CAUSE_H_

#include <string>
#include <string_view>

#include "osw/raii/fs_api.h"

namespace osw::control {

/// Two-way mapping between proto cause strings and switch_call_cause_t.
///
/// All functions are pure (no side effects, no allocation).
struct CallCause {
    /// Convert a proto cause string (e.g. "NORMAL_CLEARING") to the
    /// matching switch_call_cause_t. Unrecognised or empty strings
    /// return SWITCH_CAUSE_NORMAL_CLEARING (graceful-hangup default).
    [[nodiscard]] static switch_call_cause_t FromString(std::string_view cause_str) noexcept;

    /// Convert a switch_call_cause_t to the proto cause string
    /// (e.g. SWITCH_CAUSE_USER_BUSY → "USER_BUSY"). Unknown values
    /// return "UNSPECIFIED".
    [[nodiscard]] static std::string_view ToString(switch_call_cause_t cause) noexcept;

    // Non-constructible — static-methods-only helper.
    CallCause() = delete;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_CALL_CAUSE_H_
