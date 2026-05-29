/*
 * include/osw/security/eavesdrop_policy.h
 *
 * W7 Track A eavesdrop policy helpers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_SECURITY_EAVESDROP_POLICY_H_
#define OSW_SECURITY_EAVESDROP_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "osw/core/config.h"

namespace osw::security {

enum class EavesdropPolicy : std::uint8_t {
    kDeny = 0,
    kAudit,
    kAllow,
};

[[nodiscard]] EavesdropPolicy ParseEavesdropPolicy(std::string_view raw) noexcept;
[[nodiscard]] std::string_view EavesdropPolicyName(EavesdropPolicy policy) noexcept;
[[nodiscard]] bool IsKnownEavesdropPolicy(std::string_view raw) noexcept;

/// Resolve the effective eavesdrop policy for a tenant. The module
/// default is Config::eavesdrop_policy. Optional per-tenant overrides
/// come from Config::tenant_eavesdrop_policies in this format:
///
///   tenant_id:policy[:allow|deny];tenant2:policy[:allow|deny]
///
/// The third token is an allow_eavesdrop gate. "deny" or "false" forces
/// kDeny regardless of policy. Missing tenant entries fall back to the
/// module default.
[[nodiscard]] EavesdropPolicy ResolveEffectivePolicy(const osw::Config& config,
                                                     std::string_view tenant_id) noexcept;

/// Validate the semicolon-separated Config::tenant_eavesdrop_policies
/// string. Empty is valid.
[[nodiscard]] bool ValidateTenantEavesdropPolicies(std::string_view raw,
                                                   std::string* error = nullptr);

}  // namespace osw::security

#endif  // OSW_SECURITY_EAVESDROP_POLICY_H_
