/*
 * tests/unit/media/recording_relay_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "osw/media/recording_relay.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/stream_client.h"

#include "tests/support/media_bridge_harness.h"

namespace {

using open_switch::media::v1::AudioCodec;
using open_switch::media::v1::AudioFrame;
using open_switch::media::v1::StreamStart;
using osw::media::RecordingRelay;
using osw::media::RecordingRelayConfig;
using osw::media::StreamCallbacks;
using osw::media::StreamClient;
using osw::media::StreamConfig;

constexpr auto kWait = std::chrono::milliseconds(2000);

StreamConfig MakeStreamConfig(std::uint32_t rate_hz, std::uint32_t channels) {
    StreamConfig cfg;
    cfg.stream_id = "recording-relay-test-stream";
    cfg.channel_uuid = "chan-recording";
    cfg.tenant_id = "tenant-recording";
    cfg.purpose = StreamStart::RECORDING_RELAY;
    cfg.sample_rate_hz = rate_hz;
    cfg.channels = channels;
    cfg.codec = AudioCodec::PCM_S16LE;
    cfg.side = channels == 2 ? StreamStart::STEREO : StreamStart::BOTH_MIXED;
    cfg.send_ring_capacity_frames = 8;
    return cfg;
}

RecordingRelayConfig MakeRelayConfig(std::uint32_t rate_hz, bool stereo) {
    RecordingRelayConfig cfg;
    cfg.channel_uuid = "chan-recording";
    cfg.tenant_id = "tenant-recording";
    cfg.stream_id = "recording-relay-test";
    cfg.stereo = stereo;
    cfg.sample_rate_hz = rate_hz;
    cfg.read_fs_rate_hz = rate_hz;
    cfg.write_fs_rate_hz = rate_hz;
    cfg.desync_warn_ms = 5;
    cfg.desync_timeout_ms = 25;
    return cfg;
}

class RecordingRelayTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        ASSERT_TRUE(harness_.Start());
    }

    void TearDown() override {
        harness_.Stop();
        osw::raii::fs::MockReset();
    }

    osw::test::MediaBridgeHarness harness_;
};

TEST_F(RecordingRelayTest, MonoIgnoresReadSideAndSendsWriteSideOnly) {
    StreamClient client(harness_.MakeChannel(), MakeStreamConfig(8000, 1), StreamCallbacks{});
    ASSERT_TRUE(client.Open(1000).ok());

    RecordingRelay relay(&client, MakeRelayConfig(8000, false));
    relay.Start();

    std::vector<std::int16_t> read_samples(160, 100);
    relay.PushReadFrame(0, 8000, read_samples);
    EXPECT_FALSE(harness_.service().WaitForReceivedAudioCount(1, std::chrono::milliseconds(100)));

    std::vector<std::int16_t> write_samples(160, 400);
    relay.PushWriteFrame(0, 8000, write_samples);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));

    relay.Stop();
    client.Close();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 160u);
    EXPECT_EQ(received[0].channel(), AudioFrame::CHANNEL_UNSPECIFIED);
    const auto samples = osw::test::PayloadToSamples(received[0]);
    ASSERT_EQ(samples.size(), 160u);
    EXPECT_EQ(samples[0], 400);
}

TEST_F(RecordingRelayTest, MonoResamplesWriteSideToStreamRate) {
    StreamClient client(harness_.MakeChannel(), MakeStreamConfig(16000, 1), StreamCallbacks{});
    ASSERT_TRUE(client.Open(1000).ok());

    auto cfg = MakeRelayConfig(16000, false);
    cfg.write_fs_rate_hz = 8000;
    RecordingRelay relay(&client, std::move(cfg));
    relay.Start();

    std::vector<std::int16_t> write_samples(160, 700);
    relay.PushWriteFrame(0, 8000, write_samples);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));

    relay.Stop();
    client.Close();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 320u);
    EXPECT_EQ(received[0].payload().size(), 320u * sizeof(std::int16_t));
}

TEST_F(RecordingRelayTest, StereoPairsReadAndWriteAsInterleavedChannels) {
    StreamClient client(harness_.MakeChannel(), MakeStreamConfig(8000, 2), StreamCallbacks{});
    ASSERT_TRUE(client.Open(1000).ok());

    RecordingRelay relay(&client, MakeRelayConfig(8000, true));
    relay.Start();

    std::vector<std::int16_t> left = {1, 2, 3};
    std::vector<std::int16_t> right = {10, 20, 30};
    relay.PushReadFrame(0, 8000, left);
    relay.PushWriteFrame(0, 8000, right);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));

    relay.Stop();
    client.Close();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 3u);
    EXPECT_EQ(received[0].channel(), AudioFrame::BOTH_INTERLEAVED);
    const auto samples = osw::test::PayloadToSamples(received[0]);
    const std::vector<std::int16_t> expected = {1, 10, 2, 20, 3, 30};
    EXPECT_EQ(samples, expected);
}

}  // namespace
