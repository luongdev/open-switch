/*
 * include/osw/control/rbac.h
 *
 * osw::control::RbacRegistry — role-to-permission map and per-RPC
 * required-permission table for the W4 Auth + RBAC interceptor.
 *
 * Design:
 *   - Static boot-time table `kRpcPermissions` maps gRPC method path
 *     → required permission string.
 *   - `RbacRegistry` holds the live RBAC config: role→permissions and
 *     identity→role mappings.  Constructed from an `<auth>` XML element
 *     parsed from open_switch.conf.xml.
 *   - Reloaded atomically on SIGHUP via inotify (W4 SIGHUP mechanism):
 *     a new `RbacRegistry` is constructed from the refreshed XML and
 *     published via `std::atomic<std::shared_ptr<RbacRegistry>>` in the
 *     owning `AuthInterceptorFactory`.
 *   - `Lookup(identity, rpc_path)` is the single entry point: returns
 *     the `AuthzDecision` for a given (identity, RPC) pair.
 *
 * Thread safety:
 *   - `RbacRegistry` is immutable after construction; all methods are
 *     const and safe to call concurrently from multiple gRPC worker
 *     threads.
 *   - The `AuthInterceptorFactory` owns an
 *     `std::atomic<std::shared_ptr<RbacRegistry>>` updated by the reload
 *     path (single writer).  Each per-RPC `AuthInterceptor` snapshots the
 *     shared_ptr at construction.
 *
 * FF cited: FF-029 (gRPC ServerInterceptorFactoryInterface thread model).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_RBAC_H_
#define OSW_CONTROL_RBAC_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osw::control {

/// Result of an authorization check.
struct AuthzDecision {
    bool allowed = false;
    std::string identity;          ///< CN or JWT sub (or "anonymous").
    std::string permission_required;
    std::string deny_reason;       ///< Non-empty only when !allowed.
};

/// AuthConfig holds the parsed `<auth>` block from open_switch.conf.xml.
/// Used to construct an `RbacRegistry`; also stored in `Config`.
struct AuthConfig {
    bool require = true;           ///< <auth require="true|false">
    std::string jwt_public_key_path;  ///< <auth jwt_public_key_path="...">

    struct Role {
        std::string name;
        std::unordered_set<std::string> permissions;
    };
    std::vector<Role> roles;

    struct IdentityMapping {
        std::string identity;      ///< CN or JWT sub
        std::string role;
    };
    std::vector<IdentityMapping> identities;
};

/// Immutable RBAC registry built from an `AuthConfig`.
/// Shared across all concurrent RPC interceptor instances.
class RbacRegistry {
  public:
    /// Construct from a parsed AuthConfig.
    explicit RbacRegistry(AuthConfig config);

    /// Returns the required permission for `rpc_path`, or "" if the path
    /// is not in the permission table (treat as "health.read" fallback).
    static std::string_view RequiredPermission(std::string_view rpc_path) noexcept;

    /// Returns the role name for `identity`, or "" if unmapped.
    std::string_view RoleFor(std::string_view identity) const noexcept;

    /// Returns true iff `role` has `permission`.
    bool RoleHasPermission(std::string_view role,
                           std::string_view permission) const noexcept;

    /// Authorizes an (identity, rpc_path) pair.
    /// Encapsulates:
    ///   1. RequiredPermission lookup.
    ///   2. RoleFor(identity) lookup.
    ///   3. RoleHasPermission check.
    ///   4. require=false → allow anonymous for health.read.
    [[nodiscard]] AuthzDecision Authorize(std::string_view identity,
                                          std::string_view rpc_path) const noexcept;

    bool require() const noexcept { return config_.require; }
    const std::string& jwt_public_key_path() const noexcept {
        return config_.jwt_public_key_path;
    }

  private:
    AuthConfig config_;

    // identity → role_name (flattened from config_.identities)
    std::unordered_map<std::string, std::string> identity_to_role_;

    // role_name → set<permission> (flattened from config_.roles)
    std::unordered_map<std::string, std::unordered_set<std::string>> role_permissions_;
};

/// Parse the `<auth>` element out of an XML string (for unit tests) or
/// from the open_switch.conf.xml (production path via XmlNode).
///
/// Returns a default-deny AuthConfig (require=true, no roles/identities)
/// if `xml_text` is empty or malformed.
///
/// Production callers pass the raw XML bytes of the `<auth>` element.
/// Unit tests can pass a hand-crafted XML string.
AuthConfig ParseAuthConfig(std::string_view xml_text) noexcept;

}  // namespace osw::control

#endif  // OSW_CONTROL_RBAC_H_
