/*
 * tests/integration/w7_media_flow_integration_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/media/v1/media.pb.h"

#include "osw/media/audio_frame.h"
#include "osw/media/recording_relay.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"

#include "src/control/handlers/media_bug_callbacks.h"
#include "tests/support/media_bridge_harness.h"

namespace {

using open_switch::media::v1::AudioCodec;
using open_switch::media::v1::AudioFrame;
using open_switch::media::v1::StreamStart;
using osw::control::handlers::ReadCallbackCtx;
using osw::control::handlers::WriteCallbackCtx;
using osw::media::RecordingRelay;
using osw::media::RecordingRelayConfig;
using osw::media::StreamCallbacks;
using osw::media::StreamClient;
using osw::media::StreamConfig;
using osw::media::TtsPlayoutBuffer;

switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0x1701);
constexpr auto kWait = std::chrono::milliseconds(2000);

StreamConfig MakeStreamConfig(StreamStart::Purpose purpose,
                              std::uint32_t rate_hz,
                              std::uint32_t channels) {
    StreamConfig cfg;
    cfg.stream_id = "w7-media-flow";
    cfg.channel_uuid = "chan-w7-media";
    cfg.tenant_id = "tenant-w7";
    cfg.purpose = purpose;
    cfg.sample_rate_hz = rate_hz;
    cfg.channels = channels;
    cfg.codec = AudioCodec::PCM_S16LE;
    cfg.side = channels == 2 ? StreamStart::STEREO : StreamStart::BOTH_MIXED;
    cfg.send_ring_capacity_frames = 8;
    return cfg;
}

TtsPlayoutBuffer::Config MakeBufferConfig(std::uint32_t rate_hz) {
    TtsPlayoutBuffer::Config cfg;
    cfg.target_ms = std::chrono::milliseconds(0);
    cfg.preroll_ms = std::chrono::milliseconds(0);
    cfg.high_water_ms = std::chrono::milliseconds(1000);
    cfg.channel_sample_rate_hz = rate_hz;
    cfg.channels = 1;
    return cfg;
}

RecordingRelayConfig MakeRelayConfig(std::uint32_t rate_hz, bool stereo) {
    RecordingRelayConfig cfg;
    cfg.channel_uuid = "chan-w7-media";
    cfg.tenant_id = "tenant-w7";
    cfg.stream_id = "relay-w7-media";
    cfg.stereo = stereo;
    cfg.sample_rate_hz = rate_hz;
    cfg.read_fs_rate_hz = rate_hz;
    cfg.write_fs_rate_hz = rate_hz;
    return cfg;
}

switch_frame_t MakeFrame(std::vector<std::int16_t>* samples, std::uint32_t rate_hz) {
    switch_frame_t frame{};
    frame.data = samples->data();
    frame.datalen = static_cast<std::uint32_t>(samples->size() * sizeof(std::int16_t));
    frame.samples = static_cast<std::uint32_t>(samples->size());
    frame.rate = rate_hz;
    frame.channels = 1;
    frame.timestamp = 0;
    return frame;
}

class W7MediaFlowIntegrationTest : public ::testing::Test {
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

TEST_F(W7MediaFlowIntegrationTest, StreamingCallbacksResampleBothDirections) {
    StreamClient client(harness_.MakeChannel(),
                        MakeStreamConfig(StreamStart::STT_TRANSCRIBE, 16000, 1),
                        StreamCallbacks{});
    ASSERT_TRUE(client.Open(1000).ok());

    std::vector<std::int16_t> read_samples(160);
    std::iota(read_samples.begin(), read_samples.end(), static_cast<std::int16_t>(1));
    switch_frame_t read_frame = MakeFrame(&read_samples, 8000);
    osw::raii::fs::Mock().next_media_bug_read_frame = &read_frame;

    ReadCallbackCtx read_ctx;
    read_ctx.client = &client;
    read_ctx.fs_rate_hz = 8000;
    read_ctx.stream_rate_hz = 16000;

    ASSERT_EQ(OswStreamingReadTap(kFakeBug, &read_ctx, 1), SWITCH_TRUE);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));
    client.Close();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 320u);
    ASSERT_NE(read_ctx.resampler, nullptr);

    TtsPlayoutBuffer buffer(MakeBufferConfig(16000));
    std::vector<std::int16_t> service_samples(320, 900);
    buffer.Push(osw::media::AudioFrame(std::move(service_samples), 16000, 1, 0, 0));

    std::vector<std::int16_t> write_samples(160, 0);
    switch_frame_t write_frame = MakeFrame(&write_samples, 8000);
    osw::raii::fs::Mock().next_write_replace_frame = &write_frame;

    WriteCallbackCtx write_ctx;
    write_ctx.buffer = &buffer;
    write_ctx.stream_id = "w7-write-resample";
    write_ctx.fs_rate_hz = 8000;
    write_ctx.stream_rate_hz = 16000;

    ASSERT_EQ(OswStreamingWriteReplace(kFakeBug, &write_ctx, 3), SWITCH_TRUE);
    EXPECT_EQ(write_frame.samples, 160u);
    EXPECT_EQ(write_frame.rate, 8000u);
    ASSERT_NE(write_ctx.resampler, nullptr);
}

TEST_F(W7MediaFlowIntegrationTest, RecordingRelayMonoUsesWriteSideAndStereoInterleaves) {
    {
        StreamClient client(harness_.MakeChannel(),
                            MakeStreamConfig(StreamStart::RECORDING_RELAY, 8000, 1),
                            StreamCallbacks{});
        ASSERT_TRUE(client.Open(1000).ok());
        RecordingRelay relay(&client, MakeRelayConfig(8000, false));
        relay.Start();
        std::vector<std::int16_t> read_samples(160, 111);
        std::vector<std::int16_t> write_samples(160, 222);
        relay.PushReadFrame(0, 8000, read_samples);
        relay.PushWriteFrame(0, 8000, write_samples);
        ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));
        relay.Stop();
        client.Close();
    }

    auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 160u);
    auto samples = osw::test::PayloadToSamples(received[0]);
    ASSERT_EQ(samples.size(), 160u);
    EXPECT_EQ(samples[0], 222);

    osw::test::MediaBridgeHarness stereo_harness;
    ASSERT_TRUE(stereo_harness.Start());
    {
        StreamClient client(stereo_harness.MakeChannel(),
                            MakeStreamConfig(StreamStart::RECORDING_RELAY, 8000, 2),
                            StreamCallbacks{});
        ASSERT_TRUE(client.Open(1000).ok());
        RecordingRelay relay(&client, MakeRelayConfig(8000, true));
        relay.Start();
        std::vector<std::int16_t> left = {1, 2, 3};
        std::vector<std::int16_t> right = {10, 20, 30};
        relay.PushReadFrame(0, 8000, left);
        relay.PushWriteFrame(0, 8000, right);
        ASSERT_TRUE(stereo_harness.service().WaitForReceivedAudioCount(1, kWait));
        relay.Stop();
        client.Close();
    }

    received = stereo_harness.service().ReceivedAudioSnapshot();
    stereo_harness.Stop();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].channel(), AudioFrame::BOTH_INTERLEAVED);
    samples = osw::test::PayloadToSamples(received[0]);
    const std::vector<std::int16_t> expected = {1, 10, 2, 20, 3, 30};
    EXPECT_EQ(samples, expected);
}

}  // namespace
