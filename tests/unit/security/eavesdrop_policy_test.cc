/*
 * tests/unit/security/eavesdrop_policy_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_policy.h"

#include <gtest/gtest.h>

namespace {

using osw::security::EavesdropPolicy;

TEST(EavesdropPolicyTest, ParsesKnownValuesCaseInsensitively) {
    EXPECT_EQ(osw::security::ParseEavesdropPolicy("DENY"), EavesdropPolicy::kDeny);
    EXPECT_EQ(osw::security::ParseEavesdropPolicy("audit"), EavesdropPolicy::kAudit);
    EXPECT_EQ(osw::security::ParseEavesdropPolicy("Allow"), EavesdropPolicy::kAllow);
}

TEST(EavesdropPolicyTest, UnknownAndEmptyDefaultToDeny) {
    EXPECT_EQ(osw::security::ParseEavesdropPolicy(""), EavesdropPolicy::kDeny);
    EXPECT_EQ(osw::security::ParseEavesdropPolicy("garbage"), EavesdropPolicy::kDeny);
}

TEST(EavesdropPolicyTest, NamesAreStable) {
    EXPECT_EQ(osw::security::EavesdropPolicyName(EavesdropPolicy::kDeny), "deny");
    EXPECT_EQ(osw::security::EavesdropPolicyName(EavesdropPolicy::kAudit), "audit");
    EXPECT_EQ(osw::security::EavesdropPolicyName(EavesdropPolicy::kAllow), "allow");
}

TEST(EavesdropPolicyTest, ModuleDefaultAppliesWhenTenantUnknown) {
    osw::Config cfg;
    cfg.eavesdrop_policy = "audit";
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "unknown"), EavesdropPolicy::kAudit);
}

TEST(EavesdropPolicyTest, TenantOverrideWins) {
    osw::Config cfg;
    cfg.eavesdrop_policy = "deny";
    cfg.tenant_eavesdrop_policies = "tenant-a:allow;tenant-b:audit";
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "tenant-a"), EavesdropPolicy::kAllow);
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "tenant-b"), EavesdropPolicy::kAudit);
}

TEST(EavesdropPolicyTest, TenantOverrideTrimsWhitespace) {
    osw::Config cfg;
    cfg.eavesdrop_policy = "deny";
    cfg.tenant_eavesdrop_policies = " tenant-a : allow ; tenant-b : audit : allow ";
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "tenant-a"), EavesdropPolicy::kAllow);
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "tenant-b"), EavesdropPolicy::kAudit);
    EXPECT_TRUE(osw::security::ValidateTenantEavesdropPolicies(cfg.tenant_eavesdrop_policies));
}

TEST(EavesdropPolicyTest, TenantGateCanForceDeny) {
    osw::Config cfg;
    cfg.eavesdrop_policy = "allow";
    cfg.tenant_eavesdrop_policies = "tenant-a:allow:deny";
    EXPECT_EQ(osw::security::ResolveEffectivePolicy(cfg, "tenant-a"), EavesdropPolicy::kDeny);
}

TEST(EavesdropPolicyTest, TenantPolicyValidationRejectsBadEntries) {
    EXPECT_TRUE(osw::security::ValidateTenantEavesdropPolicies("tenant-a:audit"));
    EXPECT_FALSE(osw::security::ValidateTenantEavesdropPolicies("tenant-a"));
    EXPECT_FALSE(osw::security::ValidateTenantEavesdropPolicies("tenant-a:bad"));
    EXPECT_FALSE(osw::security::ValidateTenantEavesdropPolicies("tenant-a:audit:maybe"));
}

}  // namespace
