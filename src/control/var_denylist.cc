/*
 * src/control/var_denylist.cc
 *
 * Implementation of osw::control::IsReservedVar.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/var_denylist.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace osw::control {

namespace {

// Reserved variable-name PREFIXES (all lower-case; compared against a
// lower-cased copy of the candidate name).
constexpr std::array<std::string_view, 13> kReservedVarPrefixes{{
    "api_",
    "exec_",
    "bridge_pre_execute_",
    "bridge_post_bridge_",
    "hangup_after_bridge_",
    "sip_h_",   // injects raw SIP headers
    "record_",  // record_post_process_exec etc.
    "_record_",
    "wait_for_",
    "transfer_after_bridge",   // no trailing underscore — matches the exact var too
    "session_in_hangup_hook",  // arbitrary hangup hook injection
    "api_on_",                 // catches api_on_answer / api_on_hangup
    "execute_on_",             // catches execute_on_answer etc.
}};

}  // namespace

bool IsReservedVar(const std::string& name) noexcept {
    if (name.empty()) {
        return false;
    }
    // Build a lower-case copy for case-insensitive prefix matching.
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const std::string_view lower_sv{lower};
    for (const auto prefix : kReservedVarPrefixes) {
        if (lower_sv.substr(0, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

}  // namespace osw::control
