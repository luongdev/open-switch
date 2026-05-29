/*
 * src/security/eavesdrop_policy.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_policy.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "osw/observability/log.h"

namespace osw::security {

namespace {

constexpr const char* kSubsystem = "security.eavesdrop_policy";

std::string LowerTrim(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())) != 0) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())) != 0) {
        raw.remove_suffix(1);
    }
    std::string out(raw);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string_view TrimView(std::string_view raw) noexcept {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())) != 0) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())) != 0) {
        raw.remove_suffix(1);
    }
    return raw;
}

bool SplitTenantEntry(std::string_view entry,
                      std::string_view* tenant,
                      std::string_view* policy,
                      std::string_view* gate) noexcept {
    const std::size_t first = entry.find(':');
    if (first == std::string_view::npos || first == 0) {
        return false;
    }
    *tenant = TrimView(entry.substr(0, first));
    if (tenant->empty()) {
        return false;
    }
    const std::size_t second = entry.find(':', first + 1);
    if (second == std::string_view::npos) {
        *policy = TrimView(entry.substr(first + 1));
        *gate = {};
        return !policy->empty();
    }
    *policy = TrimView(entry.substr(first + 1, second - first - 1));
    *gate = TrimView(entry.substr(second + 1));
    return !policy->empty();
}

bool GateForcesDeny(std::string_view raw_gate) {
    const std::string gate = LowerTrim(raw_gate);
    return gate == "deny" || gate == "false" || gate == "0" || gate == "off";
}

}  // namespace

EavesdropPolicy ParseEavesdropPolicy(std::string_view raw) noexcept {
    try {
        const std::string s = LowerTrim(raw);
        if (s == "audit") {
            return EavesdropPolicy::kAudit;
        }
        if (s == "allow") {
            return EavesdropPolicy::kAllow;
        }
    } catch (...) {
    }
    return EavesdropPolicy::kDeny;
}

std::string_view EavesdropPolicyName(EavesdropPolicy policy) noexcept {
    switch (policy) {
        case EavesdropPolicy::kAudit:
            return "audit";
        case EavesdropPolicy::kAllow:
            return "allow";
        case EavesdropPolicy::kDeny:
        default:
            return "deny";
    }
}

bool IsKnownEavesdropPolicy(std::string_view raw) noexcept {
    try {
        const std::string s = LowerTrim(raw);
        return s == "deny" || s == "audit" || s == "allow";
    } catch (...) {
        return false;
    }
}

EavesdropPolicy ResolveEffectivePolicy(const osw::Config& config,
                                       std::string_view tenant_id) noexcept {
    try {
        std::string_view remaining(config.tenant_eavesdrop_policies);
        while (!remaining.empty()) {
            const std::size_t sep = remaining.find(';');
            std::string_view entry = remaining.substr(0, sep);
            remaining =
                (sep == std::string_view::npos) ? std::string_view{} : remaining.substr(sep + 1);
            while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front())) != 0) {
                entry.remove_prefix(1);
            }
            while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.back())) != 0) {
                entry.remove_suffix(1);
            }
            if (entry.empty()) {
                continue;
            }

            std::string_view tenant;
            std::string_view policy_raw;
            std::string_view gate;
            if (!SplitTenantEntry(entry, &tenant, &policy_raw, &gate)) {
                continue;
            }
            if (tenant != tenant_id) {
                continue;
            }
            if (!gate.empty() && GateForcesDeny(gate)) {
                osw::log::Debug(
                    kSubsystem,
                    "resolved eavesdrop policy tenant=%.*s policy=deny source=tenant_gate",
                    static_cast<int>(tenant_id.size()),
                    tenant_id.data());
                return EavesdropPolicy::kDeny;
            }
            const EavesdropPolicy policy = ParseEavesdropPolicy(policy_raw);
            osw::log::Debug(kSubsystem,
                            "resolved eavesdrop policy tenant=%.*s policy=%.*s "
                            "source=tenant_override",
                            static_cast<int>(tenant_id.size()),
                            tenant_id.data(),
                            static_cast<int>(EavesdropPolicyName(policy).size()),
                            EavesdropPolicyName(policy).data());
            return policy;
        }

        const EavesdropPolicy policy = ParseEavesdropPolicy(config.eavesdrop_policy);
        osw::log::Debug(kSubsystem,
                        "resolved eavesdrop policy tenant=%.*s policy=%.*s source=module_default",
                        static_cast<int>(tenant_id.size()),
                        tenant_id.data(),
                        static_cast<int>(EavesdropPolicyName(policy).size()),
                        EavesdropPolicyName(policy).data());
        return policy;
    } catch (...) {
        return EavesdropPolicy::kDeny;
    }
}

bool ValidateTenantEavesdropPolicies(std::string_view raw, std::string* error) {
    std::string_view remaining(raw);
    while (!remaining.empty()) {
        const std::size_t sep = remaining.find(';');
        std::string_view entry = remaining.substr(0, sep);
        remaining =
            (sep == std::string_view::npos) ? std::string_view{} : remaining.substr(sep + 1);
        while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front())) != 0) {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.back())) != 0) {
            entry.remove_suffix(1);
        }
        if (entry.empty()) {
            continue;
        }

        std::string_view tenant;
        std::string_view policy;
        std::string_view gate;
        if (!SplitTenantEntry(entry, &tenant, &policy, &gate)) {
            if (error) {
                *error = "tenant_eavesdrop_policies entry must be tenant:policy[:allow|deny]";
            }
            return false;
        }
        if (!IsKnownEavesdropPolicy(policy)) {
            if (error) {
                *error = "tenant_eavesdrop_policies contains unknown policy";
            }
            return false;
        }
        if (!gate.empty()) {
            const std::string g = LowerTrim(gate);
            if (g != "allow" && g != "true" && g != "1" && g != "on" && !GateForcesDeny(g)) {
                if (error) {
                    *error = "tenant_eavesdrop_policies gate must be allow/deny or true/false";
                }
                return false;
            }
        }
    }
    return true;
}

}  // namespace osw::security
