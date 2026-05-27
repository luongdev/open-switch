/*
 * tests/unit/control/rbac_test.cc
 *
 * Unit tests for RbacRegistry and ParseAuthConfig.
 *
 * Tests cover:
 *   - RequiredPermission table (known + unknown RPC path).
 *   - ParseAuthConfig from XML string: require flag, jwt_public_key_path,
 *     roles with permissions, identity→role mappings.
 *   - Authorize() — allow paths:
 *       - operator CN with operator permission.
 *       - readonly CN with health.read.
 *   - Authorize() — deny paths:
 *       - operator CN lacking permission (e.g., readonly CN for Originate).
 *       - identity not in config → no_role_for_identity.
 *       - anonymous + require=true → unauthenticated.
 *       - anonymous + require=false + health.read → allowed.
 *       - anonymous + require=false + control.originate → denied.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/rbac.h"

#include <string>

#include <gtest/gtest.h>

namespace osw::control {
namespace {

// ---------------------------------------------------------------------------
// XML fixture matching the W4 spec example.
// ---------------------------------------------------------------------------
constexpr const char* kAuthXml = R"xml(
<auth require="true" jwt_public_key_path="/etc/open_switch/jwt-es256.pub">
  <role name="operator">
    <permission>health.read</permission>
    <permission>events.subscribe</permission>
    <permission>control.originate</permission>
    <permission>control.hangup</permission>
    <permission>control.bridge</permission>
    <permission>control.execute</permission>
    <permission>control.transfer</permission>
    <permission>control.set_variables</permission>
    <permission>control.hold</permission>
  </role>
  <role name="readonly">
    <permission>health.read</permission>
    <permission>events.subscribe</permission>
  </role>
  <identity name="ops-team-cn" role="operator"/>
  <identity name="readonly-cn"  role="readonly"/>
</auth>
)xml";

// ---------------------------------------------------------------------------
// ParseAuthConfig tests
// ---------------------------------------------------------------------------

TEST(ParseAuthConfigTest, EmptyXmlReturnsDefaultDeny) {
    auto cfg = ParseAuthConfig("");
    EXPECT_TRUE(cfg.require);
    EXPECT_TRUE(cfg.roles.empty());
    EXPECT_TRUE(cfg.identities.empty());
    EXPECT_TRUE(cfg.jwt_public_key_path.empty());
}

TEST(ParseAuthConfigTest, ParsesRequireFlag) {
    auto cfg = ParseAuthConfig(kAuthXml);
    EXPECT_TRUE(cfg.require);
}

TEST(ParseAuthConfigTest, ParsesJwtPublicKeyPath) {
    auto cfg = ParseAuthConfig(kAuthXml);
    EXPECT_EQ(cfg.jwt_public_key_path, "/etc/open_switch/jwt-es256.pub");
}

TEST(ParseAuthConfigTest, ParsesTwoRoles) {
    auto cfg = ParseAuthConfig(kAuthXml);
    ASSERT_EQ(cfg.roles.size(), 2u);

    // operator role
    const auto& op = cfg.roles[0];
    EXPECT_EQ(op.name, "operator");
    EXPECT_EQ(op.permissions.count("health.read"), 1u);
    EXPECT_EQ(op.permissions.count("control.originate"), 1u);
    EXPECT_EQ(op.permissions.count("control.hold"), 1u);
    EXPECT_EQ(op.permissions.size(), 9u);

    // readonly role
    const auto& ro = cfg.roles[1];
    EXPECT_EQ(ro.name, "readonly");
    EXPECT_EQ(ro.permissions.count("health.read"), 1u);
    EXPECT_EQ(ro.permissions.count("events.subscribe"), 1u);
    EXPECT_EQ(ro.permissions.size(), 2u);
}

TEST(ParseAuthConfigTest, ParsesTwoIdentities) {
    auto cfg = ParseAuthConfig(kAuthXml);
    ASSERT_EQ(cfg.identities.size(), 2u);
    EXPECT_EQ(cfg.identities[0].identity, "ops-team-cn");
    EXPECT_EQ(cfg.identities[0].role, "operator");
    EXPECT_EQ(cfg.identities[1].identity, "readonly-cn");
    EXPECT_EQ(cfg.identities[1].role, "readonly");
}

TEST(ParseAuthConfigTest, RequireFalse) {
    constexpr const char* xml = R"(<auth require="false"/>)";
    auto cfg = ParseAuthConfig(xml);
    EXPECT_FALSE(cfg.require);
}

// ---------------------------------------------------------------------------
// RbacRegistry::RequiredPermission
// ---------------------------------------------------------------------------

TEST(RbacRegistryTest, RequiredPermissionKnownRpcs) {
    EXPECT_EQ(RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/Health"),
              "health.read");
    EXPECT_EQ(RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/Originate"),
              "control.originate");
    EXPECT_EQ(RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/HangupMany"),
              "control.hangup");
    EXPECT_EQ(RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/Hold"),
              "control.hold");
    EXPECT_EQ(RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/Unhold"),
              "control.hold");
    EXPECT_EQ(
        RbacRegistry::RequiredPermission("/open_switch.control.v1.ControlService/BlindTransfer"),
        "control.transfer");
}

TEST(RbacRegistryTest, RequiredPermissionUnknownRpcFallback) {
    // Unknown RPC → fall back to "health.read" (safe default).
    EXPECT_EQ(RbacRegistry::RequiredPermission("/unknown/Rpc"), "health.read");
}

// ---------------------------------------------------------------------------
// RbacRegistry::Authorize
// ---------------------------------------------------------------------------

class RbacAuthorizeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto cfg = ParseAuthConfig(kAuthXml);
        registry_ = std::make_unique<RbacRegistry>(std::move(cfg));
    }
    std::unique_ptr<RbacRegistry> registry_;
};

TEST_F(RbacAuthorizeTest, OperatorAllowedOriginate) {
    auto d =
        registry_->Authorize("ops-team-cn", "/open_switch.control.v1.ControlService/Originate");
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.identity, "ops-team-cn");
    EXPECT_EQ(d.permission_required, "control.originate");
    EXPECT_TRUE(d.deny_reason.empty());
}

TEST_F(RbacAuthorizeTest, OperatorAllowedHealth) {
    auto d = registry_->Authorize("ops-team-cn", "/open_switch.control.v1.ControlService/Health");
    EXPECT_TRUE(d.allowed);
}

TEST_F(RbacAuthorizeTest, ReadonlyAllowedHealth) {
    auto d = registry_->Authorize("readonly-cn", "/open_switch.control.v1.ControlService/Health");
    EXPECT_TRUE(d.allowed);
}

TEST_F(RbacAuthorizeTest, ReadonlyDeniedOriginate) {
    auto d =
        registry_->Authorize("readonly-cn", "/open_switch.control.v1.ControlService/Originate");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.permission_required, "control.originate");
    EXPECT_FALSE(d.deny_reason.empty());
}

TEST_F(RbacAuthorizeTest, ReadonlyAllowedSubscribeEvents) {
    auto d = registry_->Authorize("readonly-cn",
                                  "/open_switch.control.v1.ControlService/SubscribeEvents");
    EXPECT_TRUE(d.allowed);
}

TEST_F(RbacAuthorizeTest, UnknownIdentityDenied) {
    auto d = registry_->Authorize("unknown-cn", "/open_switch.control.v1.ControlService/Health");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.deny_reason, "no_role_for_identity");
}

TEST_F(RbacAuthorizeTest, AnonymousRequireTrueDeniedUnauthenticated) {
    auto d = registry_->Authorize("anonymous", "/open_switch.control.v1.ControlService/Health");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.deny_reason, "unauthenticated");
}

TEST(RbacAuthorizeRequireFalse, AnonymousHealthAllowed) {
    constexpr const char* xml = R"(<auth require="false">
  <role name="admin"><permission>health.read</permission></role>
</auth>)";
    auto cfg = ParseAuthConfig(xml);
    RbacRegistry reg(std::move(cfg));

    auto d = reg.Authorize("anonymous", "/open_switch.control.v1.ControlService/Health");
    EXPECT_TRUE(d.allowed);
}

TEST(RbacAuthorizeRequireFalse, AnonymousOriginateDenied) {
    constexpr const char* xml = R"(<auth require="false"/>)";
    auto cfg = ParseAuthConfig(xml);
    RbacRegistry reg(std::move(cfg));

    auto d = reg.Authorize("anonymous", "/open_switch.control.v1.ControlService/Originate");
    EXPECT_FALSE(d.allowed);
}

TEST_F(RbacAuthorizeTest, OperatorHoldAndUnhold) {
    auto dh = registry_->Authorize("ops-team-cn", "/open_switch.control.v1.ControlService/Hold");
    EXPECT_TRUE(dh.allowed);

    auto du = registry_->Authorize("ops-team-cn", "/open_switch.control.v1.ControlService/Unhold");
    EXPECT_TRUE(du.allowed);
}

}  // namespace
}  // namespace osw::control
