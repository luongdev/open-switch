/*
 * tests/unit/security/eavesdrop_detector_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_detector.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_event_t* const kBugEvent = reinterpret_cast<switch_event_t*>(0x7301);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0x7302);
switch_core_session_t* const kTargetSession = reinterpret_cast<switch_core_session_t*>(0x7303);
switch_channel_t* const kTargetChannel = reinterpret_cast<switch_channel_t*>(0x7304);

void PutInputHeader(const std::string& name, const std::string& value) {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    m.events_by_ptr[kBugEvent].headers.emplace_back(name, value);
}

const osw::raii::fs::MockState::CapturedEvent* CapturedAudit() {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(kAuditEvent);
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

class EavesdropDetectorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        auto& m = osw::raii::fs::Mock();
        m.next_session = kTargetSession;
        m.next_channel = kTargetChannel;
        m.next_event = kAuditEvent;
        m.next_bleg_uuid = "target-uuid";
        PutInputHeader("Media-Bug-Function", "eavesdrop");
        PutInputHeader("Unique-ID", "target-uuid");
    }

    void MarkBotPolicy(const std::string& policy) {
        auto& vars = osw::raii::fs::Mock().next_channel_variables;
        vars["osw_bot_session"] = "true";
        vars["osw_eavesdrop_policy"] = policy;
        vars["osw_tenant"] = "tenant-a";
        vars["osw_bot_purpose"] = "voicebot";
    }
};

TEST_F(EavesdropDetectorTest, BindAndUnbindUseMediaBugStart) {
    EXPECT_TRUE(osw::security::BindEavesdropDetector());
    EXPECT_EQ(osw::raii::fs::Mock().event_bind_calls.load(), 1);
    {
        auto& m = osw::raii::fs::Mock();
        std::lock_guard<std::mutex> g(m.capture_mu);
        ASSERT_EQ(m.bindings.size(), 1u);
        EXPECT_EQ(m.bindings[0].event, SWITCH_EVENT_MEDIA_BUG_START);
        EXPECT_EQ(m.bindings[0].id, "mod_open_switch_eavesdrop_detector");
    }
    osw::security::UnbindEavesdropDetector();
    EXPECT_EQ(osw::raii::fs::Mock().event_unbind_calls.load(), 1);
}

TEST_F(EavesdropDetectorTest, IgnoresNonEavesdropBugs) {
    osw::raii::fs::MockReset();
    osw::raii::fs::Mock().next_event = kAuditEvent;
    PutInputHeader("Media-Bug-Function", "stt_transcribe");
    PutInputHeader("Unique-ID", "target-uuid");

    osw::security::HandleMediaBugStartForTest(kBugEvent);

    EXPECT_EQ(osw::raii::fs::Mock().event_create_subclass_calls.load(), 0);
}

TEST_F(EavesdropDetectorTest, IgnoresUnmarkedSessions) {
    osw::security::HandleMediaBugStartForTest(kBugEvent);
    EXPECT_EQ(osw::raii::fs::Mock().event_create_subclass_calls.load(), 0);
}

TEST_F(EavesdropDetectorTest, EmitsAuditForBotMarkedDenyPolicy) {
    MarkBotPolicy("deny");

    osw::security::HandleMediaBugStartForTest(kBugEvent);

    const auto* event = CapturedAudit();
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->subclass_name, "osw.eavesdrop.detected_post_attach");
    EXPECT_TRUE(HasHeader(*event, "policy_applied", "deny"));
    EXPECT_TRUE(HasHeader(*event, "decision", "detected_hangup_target"));
    EXPECT_EQ(osw::raii::fs::Mock().media_bug_remove_callback_calls.load(), 0);
    EXPECT_EQ(osw::raii::fs::Mock().channel_hangup_calls.load(), 1);
    {
        auto& m = osw::raii::fs::Mock();
        std::lock_guard<std::mutex> g(m.capture_mu);
        ASSERT_EQ(m.hangup_invocations.size(), 1u);
        EXPECT_EQ(m.hangup_invocations[0].channel, kTargetChannel);
        EXPECT_EQ(m.hangup_invocations[0].cause, SWITCH_CAUSE_POLICY_REJECTED);
    }
}

TEST_F(EavesdropDetectorTest, EmitsAuditForBotMarkedAuditPolicy) {
    MarkBotPolicy("audit");

    osw::security::HandleMediaBugStartForTest(kBugEvent);

    const auto* event = CapturedAudit();
    ASSERT_NE(event, nullptr);
    EXPECT_TRUE(HasHeader(*event, "policy_applied", "audit"));
    EXPECT_TRUE(HasHeader(*event, "decision", "detected_only"));
    EXPECT_EQ(osw::raii::fs::Mock().channel_hangup_calls.load(), 0);
}

TEST_F(EavesdropDetectorTest, MissingPolicyDefaultsToDeny) {
    auto& vars = osw::raii::fs::Mock().next_channel_variables;
    vars["osw_bot_session"] = "true";

    osw::security::HandleMediaBugStartForTest(kBugEvent);

    const auto* event = CapturedAudit();
    ASSERT_NE(event, nullptr);
    EXPECT_TRUE(HasHeader(*event, "policy_applied", "deny"));
    EXPECT_TRUE(HasHeader(*event, "decision", "detected_hangup_target"));
    EXPECT_EQ(osw::raii::fs::Mock().channel_hangup_calls.load(), 1);
}

}  // namespace
