/*
 * src/security/eavesdrop_audit.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_audit.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "osw/observability/audit.h"
#include "osw/raii/fs_api.h"
#include "osw/security/eavesdrop_marker.h"

namespace osw::security {

namespace {

std::string AuditName(EavesdropPolicy policy, std::string_view layer) {
    if (layer == "2_post_attach_detection") {
        return "osw.eavesdrop.detected_post_attach";
    }
    switch (policy) {
        case EavesdropPolicy::kAudit:
            return "osw.eavesdrop.audit";
        case EavesdropPolicy::kAllow:
            return "osw.eavesdrop.allowed";
        case EavesdropPolicy::kDeny:
        default:
            return "osw.eavesdrop.denied";
    }
}

void AddHeader(std::vector<osw::audit::Header>* headers,
               std::string name,
               const char* value) {
    if (value && *value != '\0') {
        headers->push_back({std::move(name), value});
    }
}

void AddHeader(std::vector<osw::audit::Header>* headers,
               std::string name,
               std::string_view value) {
    if (!value.empty()) {
        headers->push_back({std::move(name), std::string(value)});
    }
}

}  // namespace

void EmitEavesdropAudit(switch_core_session_t* supervisor_session,
                        switch_core_session_t* target_session,
                        EavesdropPolicy policy,
                        std::string_view layer,
                        std::string_view decision) noexcept {
    try {
        switch_channel_t* target_channel = ::osw::raii::fs::SessionGetChannel(target_session);
        std::vector<osw::audit::Header> headers;
        headers.reserve(10);

        AddHeader(&headers, "target_uuid", ::osw::raii::fs::SessionGetUuid(target_session));
        if (target_channel) {
            AddHeader(&headers,
                      "target_tenant",
                      ::osw::raii::fs::ChannelGetVariable(target_channel, kTenantVar));
            AddHeader(&headers,
                      "target_bot_purpose",
                      ::osw::raii::fs::ChannelGetVariable(target_channel, kBotPurposeVar));
        }
        AddHeader(&headers, "policy_applied", EavesdropPolicyName(policy));
        AddHeader(&headers, "decision", decision);
        AddHeader(&headers, "layer", layer);

        switch_channel_t* supervisor_channel =
            ::osw::raii::fs::SessionGetChannel(supervisor_session);
        if (supervisor_channel) {
            AddHeader(&headers,
                      "supervisor_identity",
                      ::osw::raii::fs::ChannelGetVariable(supervisor_channel, "sip_from_uri"));
            AddHeader(&headers,
                      "supervisor_ip",
                      ::osw::raii::fs::ChannelGetVariable(supervisor_channel, "sip_network_ip"));
            AddHeader(&headers, "Unique-ID", ::osw::raii::fs::SessionGetUuid(supervisor_session));
        } else {
            AddHeader(&headers, "Unique-ID", ::osw::raii::fs::SessionGetUuid(target_session));
        }

        (void)osw::audit::EmitSubclass(AuditName(policy, layer), headers);
    } catch (...) {
    }
}

}  // namespace osw::security
