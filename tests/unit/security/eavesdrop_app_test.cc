/*
 * tests/unit/security/eavesdrop_app_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_app.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_core_session_t* const kSupervisorSession = reinterpret_cast<switch_core_session_t*>(0x7201);
switch_core_session_t* const kTargetSession = reinterpret_cast<switch_core_session_t*>(0x7202);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0x7203);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0x7204);

const osw::raii::fs::MockState::CapturedEvent* CapturedEvent(switch_event_t* ptr) {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(ptr);
    if (it == m.events_by_ptr.end()) {
        return nullptr;
    }
    return &it->second;
}

bool HasHeader(const osw::raii::fs::MockState::CapturedEvent& event,
               const std::string& name,
               const std::string& value) {
    return std::any_of(event.headers.begin(), event.headers.end(), [&](const auto& kv) {
        return kv.first == name && kv.second == value;
    });
}

class EavesdropAppTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        auto& m = osw::raii::fs::Mock();
        m.next_session = kTargetSession;
        m.next_channel = kChannel;
        m.next_event = kAuditEvent;
        m.next_bleg_uuid = "target-uuid";
    }

    void MarkBotPolicy(const std::string& policy) {
        auto& vars = osw::raii::fs::Mock().next_channel_variables;
        vars["osw_bot_session"] = "true";
        vars["osw_eavesdrop_policy"] = policy;
        vars["osw_tenant"] = "tenant-a";
        vars["osw_bot_purpose"] = "voicebot";
    }
};

TEST_F(EavesdropAppTest, DenyHangsUpSupervisorAndAudits) {
    MarkBotPolicy("deny");

    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "target-uuid");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 1);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 0);
    EXPECT_EQ(m.event_create_subclass_calls.load(), 1);

    const auto* event = CapturedEvent(kAuditEvent);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->subclass_name, "osw.eavesdrop.denied");
    EXPECT_TRUE(HasHeader(*event, "policy_applied", "deny"));
    EXPECT_TRUE(HasHeader(*event, "decision", "hangup"));
}

TEST_F(EavesdropAppTest, AuditPolicyDelegatesAndAudits) {
    MarkBotPolicy("audit");

    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "target-uuid");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 1);
    ASSERT_EQ(m.ivr_eavesdrop_session_invocations.size(), 1u);
    EXPECT_EQ(m.ivr_eavesdrop_session_invocations[0].target_uuid, "target-uuid");

    const auto* event = CapturedEvent(kAuditEvent);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->subclass_name, "osw.eavesdrop.audit");
    EXPECT_TRUE(HasHeader(*event, "decision", "permitted"));
}

TEST_F(EavesdropAppTest, AllowPolicyDelegatesAndAudits) {
    MarkBotPolicy("allow");

    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "target-uuid");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 1);

    const auto* event = CapturedEvent(kAuditEvent);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->subclass_name, "osw.eavesdrop.allowed");
}

TEST_F(EavesdropAppTest, UnmarkedTargetDelegatesWithoutAudit) {
    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "target-uuid");

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 1);
    EXPECT_EQ(m.event_create_subclass_calls.load(), 0);
}

TEST_F(EavesdropAppTest, MissingDataIsNoOp) {
    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "");
    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 0);
    EXPECT_EQ(m.event_create_subclass_calls.load(), 0);
}

TEST_F(EavesdropAppTest, TargetNotFoundIsNoOp) {
    osw::raii::fs::Mock().next_session = nullptr;
    osw::security::InvokeOswEavesdropForTest(kSupervisorSession, "missing");
    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.channel_hangup_calls.load(), 0);
    EXPECT_EQ(m.ivr_eavesdrop_session_calls.load(), 0);
    EXPECT_EQ(m.event_create_subclass_calls.load(), 0);
}

TEST_F(EavesdropAppTest, NullSupervisorSessionIsNoOp) {
    MarkBotPolicy("deny");
    osw::security::InvokeOswEavesdropForTest(nullptr, "target-uuid");
    EXPECT_EQ(osw::raii::fs::Mock().channel_hangup_calls.load(), 0);
}

}  // namespace
