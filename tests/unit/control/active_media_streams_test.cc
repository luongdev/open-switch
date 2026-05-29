/*
 * tests/unit/control/active_media_streams_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "osw/control/active_media_streams.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/media/recording_relay.h"

namespace {

constexpr const char* kChannelUuid = "active-streams-channel-0001";
constexpr const char* kTenantId = "tenant-a";
constexpr const char* kStreamId = "stream-collision";
switch_core_session_t* const kFakeSession = reinterpret_cast<switch_core_session_t*>(0x7D01);
switch_media_bug_t* const kInjectBug = reinterpret_cast<switch_media_bug_t*>(0x7D02);
switch_media_bug_t* const kRelayBug = reinterpret_cast<switch_media_bug_t*>(0x7D03);
switch_event_t* const kAuditEvent = reinterpret_cast<switch_event_t*>(0x7D04);

using CapturedEvent = osw::raii::fs::MockState::CapturedEvent;

std::optional<CapturedEvent> CapturedEventCopy(switch_event_t* ptr) {
    auto& m = osw::raii::fs::Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(ptr);
    if (it == m.events_by_ptr.end()) {
        return std::nullopt;
    }
    return it->second;
}

class ActiveMediaStreamsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        auto& m = osw::raii::fs::Mock();
        m.next_session = kFakeSession;
        m.next_bleg_uuid = kChannelUuid;
        m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
        m.next_event = kAuditEvent;

        bug_mgr_ = std::make_unique<osw::media::MediaBugManager>();
        streams_ = std::make_unique<osw::control::ActiveMediaStreams>();
    }

    void TearDown() override {
        inject_bugs_.clear();
        streams_.reset();
        bug_mgr_.reset();
        osw::raii::fs::MockReset();
    }

    void PrimeInjectBug() {
        osw::raii::fs::Mock().next_bug = kInjectBug;

        osw::media::BugConfig cfg;
        cfg.purpose = osw::media::Purpose::kTtsPlayback;
        cfg.fs_flags = SMBF_WRITE_REPLACE;
        cfg.target_rate_hz = 16000;
        cfg.tenant_id = kTenantId;
        cfg.stream_endpoint = "tts-upstream";

        auto attached = bug_mgr_->Attach(kFakeSession, std::move(cfg));
        ASSERT_TRUE(attached.ok) << attached.error;
        inject_bugs_.push_back(std::move(attached.handle));
    }

    osw::media::BugHandle AttachRelayBug() {
        osw::raii::fs::Mock().next_bug = kRelayBug;

        osw::media::BugConfig cfg;
        cfg.purpose = osw::media::Purpose::kRecordingRelay;
        cfg.fs_flags = SMBF_READ_STREAM;
        cfg.target_rate_hz = 16000;
        cfg.tenant_id = kTenantId;
        cfg.stream_endpoint = "recording-upstream";

        auto attached = bug_mgr_->Attach(kFakeSession, std::move(cfg));
        EXPECT_TRUE(attached.ok) << attached.error;
        return std::move(attached.handle);
    }

    std::unique_ptr<osw::control::ActiveMediaStream> MakeCollisionSentinel() {
        auto stream = std::make_unique<osw::control::ActiveMediaStream>();
        stream->channel_uuid = "other-channel";
        stream->stream_id = kStreamId;
        stream->purpose = open_switch::media::v1::StreamStart::TTS_PLAYBACK;
        return stream;
    }

    std::unique_ptr<osw::control::ActiveMediaStream> MakeRecordingRelayCandidate() {
        auto stream = std::make_unique<osw::control::ActiveMediaStream>();
        stream->channel_uuid = kChannelUuid;
        stream->stream_id = kStreamId;
        stream->purpose = open_switch::media::v1::StreamStart::RECORDING_RELAY;
        stream->bugs.push_back(AttachRelayBug());

        osw::media::RecordingRelayConfig cfg;
        cfg.channel_uuid = kChannelUuid;
        cfg.tenant_id = kTenantId;
        cfg.stream_id = kStreamId;
        auto relay = std::make_unique<osw::media::RecordingRelay>(nullptr, std::move(cfg));
        stream->recording_ctx =
            std::unique_ptr<void, osw::control::RecordingCtxDeleter>(relay.release());
        return stream;
    }

    std::unique_ptr<osw::media::MediaBugManager> bug_mgr_;
    std::unique_ptr<osw::control::ActiveMediaStreams> streams_;
    std::vector<osw::media::BugHandle> inject_bugs_;
};

TEST_F(ActiveMediaStreamsTest, InsertCollisionTearsDownRejectedRecordingRelayBeforeFree) {
    ASSERT_TRUE(streams_->Insert(MakeCollisionSentinel()));
    PrimeInjectBug();
    auto candidate = MakeRecordingRelayCandidate();

    const int removes_before = osw::raii::fs::Mock().media_bug_remove_calls.load();
    const int events_before = osw::raii::fs::Mock().event_create_subclass_calls.load();

    EXPECT_FALSE(streams_->Insert(std::move(candidate)));

    EXPECT_EQ(streams_->Size(), 1u);
    EXPECT_EQ(osw::raii::fs::Mock().media_bug_remove_calls.load(), removes_before + 1);
    EXPECT_EQ(osw::raii::fs::Mock().event_create_subclass_calls.load(), events_before + 1);

    auto event = CapturedEventCopy(kAuditEvent);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->subclass_name, "osw.recording.relay_stopped");
}

}  // namespace
