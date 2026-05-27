/*
 * src/control/rbac.cc — RbacRegistry implementation.
 *
 * Provides:
 *   - Static kRpcPermissions table mapping gRPC method path → permission.
 *   - RbacRegistry construction from AuthConfig.
 *   - ParseAuthConfig — minimal expat-free XML parser for the <auth> block.
 *
 * ParseAuthConfig uses a hand-rolled tokenizer rather than pulling libxml2
 * or expat: the <auth> block schema is small (< 10 element types) and the
 * FS XML facility (switch_xml_t) is FS-dependent — unit tests of the RBAC
 * logic must run without a FreeSWITCH library.  The tokenizer supports:
 *   - <auth require="..." jwt_public_key_path="...">
 *   - <role name="..."> ... </role>
 *   - <permission>..text..</permission>
 *   - <identity name="..." role="..."/>
 *
 * FF cited: FF-029 (interceptor thread model).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/rbac.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "osw/observability/log.h"

namespace osw::control {

namespace {

constexpr const char* kSubsystem = "control.rbac";

// ---------------------------------------------------------------------------
// Per-RPC required permission table (V1 fixed, from spec).
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, std::string> kRpcPermissions = {
    {"/open_switch.control.v1.ControlService/Health", "health.read"},
    {"/open_switch.control.v1.ControlService/SubscribeEvents", "events.subscribe"},
    {"/open_switch.control.v1.ControlService/Originate", "control.originate"},
    {"/open_switch.control.v1.ControlService/Hangup", "control.hangup"},
    {"/open_switch.control.v1.ControlService/HangupMany", "control.hangup"},
    {"/open_switch.control.v1.ControlService/Bridge", "control.bridge"},
    {"/open_switch.control.v1.ControlService/Execute", "control.execute"},
    {"/open_switch.control.v1.ControlService/BlindTransfer", "control.transfer"},
    {"/open_switch.control.v1.ControlService/SetVariables", "control.set_variables"},
    {"/open_switch.control.v1.ControlService/Hold", "control.hold"},
    {"/open_switch.control.v1.ControlService/Unhold", "control.hold"},
};

// ---------------------------------------------------------------------------
// Minimal XML attribute extractor.
// Extracts the value of `attr_name` from an XML open-tag string like:
//   <role name="operator">
// Returns "" if the attribute is not found.
// ---------------------------------------------------------------------------
std::string ExtractAttr(std::string_view tag, std::string_view attr_name) {
    // Look for attr_name followed by ="..."  or  ='...'
    std::string needle;
    needle.reserve(attr_name.size() + 2);
    needle.append(attr_name);
    needle += '=';

    auto pos = tag.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    if (pos >= tag.size()) {
        return {};
    }
    char quote = tag[pos];
    if (quote != '"' && quote != '\'') {
        return {};
    }
    ++pos;
    auto end = tag.find(quote, pos);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(tag.substr(pos, end - pos));
}

// Trim whitespace from both ends of a string_view.
std::string_view Trim(std::string_view sv) {
    while (!sv.empty() && static_cast<unsigned char>(sv.front()) <= ' ') {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && static_cast<unsigned char>(sv.back()) <= ' ') {
        sv.remove_suffix(1);
    }
    return sv;
}

// Case-insensitive compare for small strings (attribute values).
bool IequalsAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// RbacRegistry
// ---------------------------------------------------------------------------

RbacRegistry::RbacRegistry(AuthConfig config) : config_(std::move(config)) {
    for (const auto& role : config_.roles) {
        role_permissions_[role.name] = role.permissions;
    }
    for (const auto& id : config_.identities) {
        identity_to_role_[id.identity] = id.role;
    }
    osw::log::Debug(kSubsystem,
                    "RbacRegistry built: require=%d roles=%zu identities=%zu",
                    static_cast<int>(config_.require),
                    config_.roles.size(),
                    config_.identities.size());
}

// static
std::string_view RbacRegistry::RequiredPermission(std::string_view rpc_path) noexcept {
    auto it = kRpcPermissions.find(std::string(rpc_path));
    if (it == kRpcPermissions.end()) {
        // Unknown path — require health.read as a safe default.
        return "health.read";
    }
    return it->second;
}

std::string_view RbacRegistry::RoleFor(std::string_view identity) const noexcept {
    auto it = identity_to_role_.find(std::string(identity));
    if (it == identity_to_role_.end()) {
        return {};
    }
    return it->second;
}

bool RbacRegistry::RoleHasPermission(std::string_view role,
                                     std::string_view permission) const noexcept {
    auto it = role_permissions_.find(std::string(role));
    if (it == role_permissions_.end()) {
        return false;
    }
    return it->second.count(std::string(permission)) > 0;
}

AuthzDecision RbacRegistry::Authorize(std::string_view identity,
                                      std::string_view rpc_path) const noexcept {
    AuthzDecision result;
    result.identity = std::string(identity);
    result.permission_required = std::string(RequiredPermission(rpc_path));

    const bool is_anonymous = (identity == "anonymous");

    // Anonymous + require=false → allow only health.read.
    if (is_anonymous && !config_.require) {
        if (result.permission_required == "health.read") {
            result.allowed = true;
            return result;
        }
        result.allowed = false;
        result.deny_reason = "anonymous:permission_denied";
        return result;
    }

    // Anonymous + require=true → unauthenticated.
    if (is_anonymous) {
        result.allowed = false;
        result.deny_reason = "unauthenticated";
        return result;
    }

    // Look up role for identity.
    auto role = RoleFor(identity);
    if (role.empty()) {
        result.allowed = false;
        result.deny_reason = "no_role_for_identity";
        return result;
    }

    if (RoleHasPermission(role, result.permission_required)) {
        result.allowed = true;
        return result;
    }

    result.allowed = false;
    result.deny_reason = "permission_denied:" + result.permission_required;
    return result;
}

// ---------------------------------------------------------------------------
// ParseAuthConfig — hand-rolled tokenizer for the <auth> XML block.
// ---------------------------------------------------------------------------

AuthConfig ParseAuthConfig(std::string_view xml_text) noexcept {
    AuthConfig cfg;
    // Default: require=true, no roles/identities.
    cfg.require = true;

    if (xml_text.empty()) {
        osw::log::Warn(kSubsystem, "ParseAuthConfig: empty XML; using default-deny config");
        return cfg;
    }

    std::string_view text = xml_text;
    AuthConfig::Role* current_role = nullptr;

    // Simple token loop: find '<', extract tag content up to '>'.
    while (!text.empty()) {
        auto lt = text.find('<');
        if (lt == std::string_view::npos)
            break;
        text.remove_prefix(lt + 1);  // skip past '<'

        auto gt = text.find('>');
        if (gt == std::string_view::npos)
            break;

        std::string_view tag_content = text.substr(0, gt);
        std::string_view remainder = text.substr(gt + 1);

        // Detect tag name (first token in tag_content).
        std::string_view trimmed = Trim(tag_content);

        // Skip XML declaration / comments.
        if (trimmed.starts_with('!') || trimmed.starts_with('?')) {
            text = remainder;
            continue;
        }

        // Closing tag?
        bool is_close = trimmed.starts_with('/');
        if (is_close) {
            trimmed.remove_prefix(1);
            // Extract tag name.
            auto sp = trimmed.find_first_of(" \t\r\n/>");
            std::string_view tname =
                (sp == std::string_view::npos) ? trimmed : trimmed.substr(0, sp);
            if (tname == "role") {
                current_role = nullptr;
            }
            text = remainder;
            continue;
        }

        // Self-closing tag ends with '/'.
        bool self_closing = trimmed.ends_with('/');
        if (self_closing) {
            trimmed.remove_suffix(1);
        }

        // Extract tag name.
        auto sp = trimmed.find_first_of(" \t\r\n");
        std::string_view tag_name =
            (sp == std::string_view::npos) ? trimmed : trimmed.substr(0, sp);
        std::string_view attrs =
            (sp == std::string_view::npos) ? std::string_view{} : trimmed.substr(sp);

        if (tag_name == "auth") {
            // Parse require= and jwt_public_key_path=
            std::string req_val = ExtractAttr(attrs, "require");
            if (!req_val.empty()) {
                cfg.require = IequalsAscii(req_val, "true") || req_val == "1";
            }
            std::string jk = ExtractAttr(attrs, "jwt_public_key_path");
            if (!jk.empty()) {
                cfg.jwt_public_key_path = std::move(jk);
            }
        } else if (tag_name == "role") {
            std::string rname = ExtractAttr(attrs, "name");
            if (!rname.empty()) {
                cfg.roles.push_back(AuthConfig::Role{std::move(rname), {}});
                current_role = &cfg.roles.back();
            }
        } else if (tag_name == "permission") {
            // Content is between this tag and </permission>.
            if (current_role != nullptr) {
                // Find text content between <permission> and </permission>.
                auto end_tag = remainder.find("</permission>");
                if (end_tag != std::string_view::npos) {
                    std::string perm(Trim(remainder.substr(0, end_tag)));
                    if (!perm.empty()) {
                        current_role->permissions.insert(std::move(perm));
                    }
                    // Advance past </permission>.
                    remainder.remove_prefix(end_tag + sizeof("</permission>") - 1);
                }
            }
        } else if (tag_name == "identity") {
            std::string iname = ExtractAttr(attrs, "name");
            std::string irole = ExtractAttr(attrs, "role");
            if (!iname.empty() && !irole.empty()) {
                cfg.identities.push_back(
                    AuthConfig::IdentityMapping{std::move(iname), std::move(irole)});
            }
        }

        text = remainder;
    }

    return cfg;
}

}  // namespace osw::control
