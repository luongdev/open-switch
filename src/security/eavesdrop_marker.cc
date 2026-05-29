/*
 * src/security/eavesdrop_marker.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_marker.h"

#include <string>
#include <string_view>

#include "osw/raii/fs_api.h"

namespace osw::security {

namespace {

void SetChannelVar(switch_channel_t* channel, const char* name, std::string_view value) {
    const std::string value_s(value);
    (void)::osw::raii::fs::ChannelSetVariable(channel, name, value_s.c_str());
}

}  // namespace

void MarkBotSession(switch_core_session_t* session,
                    std::string_view purpose,
                    EavesdropPolicy policy,
                    std::string_view tenant_id) noexcept {
    try {
        switch_channel_t* channel = ::osw::raii::fs::SessionGetChannel(session);
        if (!channel) {
            return;
        }
        SetChannelVar(channel, kBotSessionVar, "true");
        SetChannelVar(channel, kBotPurposeVar, purpose);
        SetChannelVar(channel, kEavesdropPolicyVar, EavesdropPolicyName(policy));
        SetChannelVar(channel, kTenantVar, tenant_id);
    } catch (...) {
    }
}

}  // namespace osw::security
