/*
 * tests/unit/media/silence_driver_test.cc
 *
 * Unit tests for W6.6 osw::media::SilenceDriverRegistry.
 * Uses the FS-mock seam (OSW_TEST_FS_MOCK=1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// FS mock seam MUST be included first.
// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "osw/media/silence_driver.h"

#include <cstdint>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "osw/core/config.h"
#include "osw/media/bug_handle.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"

namespace {

using osw::media::BugConfig;
using osw::media::BugHandle;
using osw::media::MediaBugManager;
using osw::media::Purpose;
using osw::media::SilenceDriverRegistry;
using osw::raii::fs::Mock;
using osw::raii::fs::MockReset;

switch_core_session_t* FakeSession(std::uintptr_t v = 0x1) {
    return reinterpret_cast<switch_core_session_t*>(v);
}

switch_channel_t* FakeChannel(std::uintptr_t v = 0x2) {
    return reinterpret_cast<switch_channel_t*>(v);
}

switch_media_bug_t* FakeBug(std::uintptr_t v = 0x3) {
    return reinterpret_cast<switch_media_bug_t*>(v);
}

switch_event_t* FakeEvent(std::uintptr_t v = 0x4) {
    return reinterpret_cast<switch_event_t*>(v);
}

class SilenceDriverTest : public ::testing::Test {
  protected:
    void SetUp() override {
        MockReset();
        Mock().next_bleg_uuid = "silence-driver-channel-1";
        Mock().next_session = FakeSession();
        Mock().next_channel = FakeChannel();
        Mock().next_bug = FakeBug();
        Mock().next_event = FakeEvent();
    }

    void TearDown() override { MockReset(); }
};

TEST_F(SilenceDriverTest, StartsOnParkedWriteReplaceChannel) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(1, Mock().ivr_broadcast_calls.load());
    EXPECT_EQ(1u, registry.ActiveCount());

    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        ASSERT_EQ(1u, Mock().ivr_broadcast_invocations.size());
        EXPECT_EQ("silence-driver-channel-1", Mock().ivr_broadcast_invocations[0].uuid);
        EXPECT_EQ("silence_stream://-1", Mock().ivr_broadcast_invocations[0].path);
        EXPECT_EQ(static_cast<switch_media_flag_t>(SMF_ECHO_ALEG | SMF_PRIORITY),
                  Mock().ivr_broadcast_invocations[0].flags);
    }

    registry.DrainAll();
    EXPECT_EQ(0u, registry.ActiveCount());
}

TEST_F(SilenceDriverTest, RepeatAttachIsIdempotent) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());
    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(1u, registry.ActiveCount());
    EXPECT_EQ(1, Mock().ivr_broadcast_calls.load());

    registry.DrainAll();
}

TEST_F(SilenceDriverTest, DetachIfOrphanStopsDriver) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());
    ASSERT_EQ(1, Mock().ivr_broadcast_calls.load());

    registry.DetachIfOrphan("silence-driver-channel-1");

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(1, Mock().channel_set_flag_calls.load());
    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        ASSERT_EQ(1u, Mock().set_flag_invocations.size());
        EXPECT_EQ(CF_BREAK, Mock().set_flag_invocations[0].flag);
    }
}

TEST_F(SilenceDriverTest, SkipsWhenChannelBroadcasting) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    Mock().next_channel_flags = CF_BROADCAST;

    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
}

TEST_F(SilenceDriverTest, SkipsWhenChannelBridged) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    Mock().next_channel_flags = CF_BRIDGED;

    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
}

TEST_F(SilenceDriverTest, SkipsWhenChannelAlreadyRunningPlayback) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    Mock().next_channel_variables["current_application"] = "playback";

    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
}

TEST_F(SilenceDriverTest, DisabledByConfigDoesNotStart) {
    osw::Config cfg;
    cfg.silence_driver_enabled = false;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
}

TEST_F(SilenceDriverTest, CapReachedRefusesAndAudits) {
    osw::Config cfg;
    cfg.max_silence_drivers = 1;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());
    ASSERT_EQ(1, Mock().ivr_broadcast_calls.load());

    Mock().next_bleg_uuid = "silence-driver-channel-2";
    registry.AttachOpportunistic(FakeSession(0x11));

    EXPECT_EQ(1u, registry.ActiveCount());
    EXPECT_EQ(1, Mock().ivr_broadcast_calls.load());

    {
        std::lock_guard<std::mutex> g(Mock().capture_mu);
        auto it = Mock().events_by_ptr.find(FakeEvent());
        ASSERT_NE(Mock().events_by_ptr.end(), it);
        EXPECT_EQ("osw.audit.osw.media.silence_driver.cap_reached", it->second.subclass_name);
    }

    registry.DrainAll();
}

TEST_F(SilenceDriverTest, DrainAllStopsEveryDriver) {
    osw::Config cfg;
    cfg.max_silence_drivers = 2;
    SilenceDriverRegistry registry(cfg);

    registry.AttachOpportunistic(FakeSession());
    ASSERT_EQ(1, Mock().ivr_broadcast_calls.load());

    Mock().next_bleg_uuid = "silence-driver-channel-2";
    registry.AttachOpportunistic(FakeSession(0x11));
    ASSERT_EQ(2, Mock().ivr_broadcast_calls.load());
    EXPECT_EQ(2u, registry.ActiveCount());

    registry.DrainAll();

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_GE(Mock().channel_set_flag_calls.load(), 1);
}

TEST_F(SilenceDriverTest, MediaBugManagerStartsAndStopsOnWriteReplaceLifecycle) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    MediaBugManager manager;
    manager.SetSilenceDriverRegistry(&registry);

    BugConfig tts{Purpose::kTtsPlayback, SMBF_WRITE_REPLACE, 8000, "tenant", "ep"};
    auto attached = manager.Attach(FakeSession(), tts);
    ASSERT_TRUE(attached.ok) << attached.error;
    ASSERT_EQ(1, Mock().ivr_broadcast_calls.load());
    EXPECT_EQ(1u, registry.ActiveCount());

    attached.handle = BugHandle{};

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(1, Mock().channel_set_flag_calls.load());
}

TEST_F(SilenceDriverTest, MediaBugManagerSkipsSilenceDriverDuringForegroundPlayback) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    MediaBugManager manager;
    manager.SetSilenceDriverRegistry(&registry);
    Mock().next_channel_variables["current_application"] = "playback";

    BugConfig tts{Purpose::kTtsPlayback, SMBF_WRITE_REPLACE, 8000, "tenant", "ep"};
    auto attached = manager.Attach(FakeSession(), tts);
    ASSERT_TRUE(attached.ok) << attached.error;

    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
    EXPECT_EQ(0u, registry.ActiveCount());

    attached.handle = BugHandle{};

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().channel_set_flag_calls.load());
}

TEST_F(SilenceDriverTest, MediaBugManagerKeepsDriverUntilLastWriteReplaceDetaches) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    MediaBugManager manager;
    manager.SetSilenceDriverRegistry(&registry);

    auto tts = manager.Attach(
        FakeSession(), {Purpose::kTtsPlayback, SMBF_WRITE_REPLACE, 8000, "tenant", "ep"});
    ASSERT_TRUE(tts.ok) << tts.error;
    ASSERT_EQ(1, Mock().ivr_broadcast_calls.load());

    auto voicebot_write = manager.Attach(FakeSession(),
                                         {Purpose::kVoicebotDuplexWrite,
                                          SMBF_WRITE_REPLACE,
                                          8000,
                                          "tenant",
                                          "ep"});
    ASSERT_TRUE(voicebot_write.ok) << voicebot_write.error;
    EXPECT_EQ(1u, registry.ActiveCount());

    tts.handle = BugHandle{};
    EXPECT_EQ(1u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().channel_set_flag_calls.load());

    voicebot_write.handle = BugHandle{};
    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(1, Mock().channel_set_flag_calls.load());
}

TEST_F(SilenceDriverTest, MediaBugManagerReadOnlyAttachDoesNotStart) {
    osw::Config cfg;
    SilenceDriverRegistry registry(cfg);
    MediaBugManager manager;
    manager.SetSilenceDriverRegistry(&registry);

    BugConfig stt{Purpose::kSttTranscribe, SMBF_READ_STREAM, 8000, "tenant", "ep"};
    auto attached = manager.Attach(FakeSession(), stt);
    ASSERT_TRUE(attached.ok) << attached.error;

    EXPECT_EQ(0u, registry.ActiveCount());
    EXPECT_EQ(0, Mock().ivr_broadcast_calls.load());
}

}  // namespace
