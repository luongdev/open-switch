/*
 * tests/unit/security/eavesdrop_marker_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_marker.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "osw/raii/fs_mock.h"

namespace {

switch_core_session_t* const kSession = reinterpret_cast<switch_core_session_t*>(0x7101);
switch_channel_t* const kChannel = reinterpret_cast<switch_channel_t*>(0x7102);

bool HasSetVariable(const std::string& name, const std::string& value) {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    return std::any_of(
        m.set_variable_invocations.begin(), m.set_variable_invocations.end(), [&](const auto& cap) {
            return cap.channel == kChannel && cap.name == name && cap.value == value;
        });
}

class EavesdropMarkerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        osw::raii::fs::Mock().next_channel = kChannel;
    }
};

TEST_F(EavesdropMarkerTest, MarkBotSessionSetsExpectedChannelVariables) {
    osw::security::MarkBotSession(
        kSession, "voicebot", osw::security::EavesdropPolicy::kAudit, "tenant-a");

    EXPECT_EQ(osw::raii::fs::Mock().channel_set_variable_calls.load(), 4);
    EXPECT_TRUE(HasSetVariable("osw_bot_session", "true"));
    EXPECT_TRUE(HasSetVariable("osw_bot_purpose", "voicebot"));
    EXPECT_TRUE(HasSetVariable("osw_eavesdrop_policy", "audit"));
    EXPECT_TRUE(HasSetVariable("osw_tenant", "tenant-a"));
}

TEST_F(EavesdropMarkerTest, NullSessionIsNoOp) {
    osw::security::MarkBotSession(
        nullptr, "tts", osw::security::EavesdropPolicy::kDeny, "tenant-a");
    EXPECT_EQ(osw::raii::fs::Mock().channel_set_variable_calls.load(), 0);
}

}  // namespace
