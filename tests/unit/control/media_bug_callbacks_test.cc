/*
 * tests/unit/control/media_bug_callbacks_test.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// clang-format off
#include "osw/raii/fs_mock.h"  // IWYU pragma: keep
// clang-format on

#include "src/control/handlers/media_bug_callbacks.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/media/v1/media.pb.h"

#include "osw/media/audio_frame.h"
#include "osw/media/bot_session.h"
#include "osw/media/stream_client.h"
#include "osw/media/tts_playout_buffer.h"

#include "tests/support/media_bridge_harness.h"

namespace {

using open_switch::control::v1::StartBotRequest;
using open_switch::media::v1::AudioCodec;
using open_switch::media::v1::StreamStart;
using osw::control::handlers::ReadCallbackCtx;
using osw::control::handlers::WriteCallbackCtx;
using osw::media::AudioFrame;
using osw::media::BotReadTapCtx;
using osw::media::BotSession;
using osw::media::BotSessionConfig;
using osw::media::BotWriteReplaceCtx;
using osw::media::StreamCallbacks;
using osw::media::StreamClient;
using osw::media::StreamConfig;
using osw::media::TtsPlayoutBuffer;

switch_media_bug_t* const kFakeBug = reinterpret_cast<switch_media_bug_t*>(0xC601);

constexpr auto kWait = std::chrono::milliseconds(2000);

StreamConfig MakeStreamConfig(std::uint32_t rate_hz) {
    StreamConfig cfg;
    cfg.stream_id = "callback-test-stream";
    cfg.channel_uuid = "chan-callback";
    cfg.tenant_id = "tenant-callback";
    cfg.purpose = StreamStart::STT_TRANSCRIBE;
    cfg.sample_rate_hz = rate_hz;
    cfg.channels = 1;
    cfg.codec = AudioCodec::PCM_S16LE;
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

BotSessionConfig MakeBotConfig(StartBotRequest::Purpose purpose, std::uint32_t rate_hz) {
    BotSessionConfig cfg;
    cfg.bot_id = "bot-callback";
    cfg.tenant_id = "tenant-callback";
    cfg.target_channel_uuids = {"chan-a"};
    cfg.purpose = purpose;
    cfg.sample_rate_hz = rate_hz;
    cfg.target_queue_ms = 100;
    return cfg;
}

switch_frame_t MakeFrame(std::vector<std::int16_t>* samples, std::uint32_t rate_hz) {
    switch_frame_t frame{};
    frame.data = samples->data();
    frame.datalen = static_cast<std::uint32_t>(samples->size() * sizeof(std::int16_t));
    frame.samples = static_cast<std::uint32_t>(samples->size());
    frame.rate = rate_hz;
    frame.channels = 1;
    frame.timestamp = 123;
    return frame;
}

class MediaBugCallbacksTest : public ::testing::Test {
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

TEST_F(MediaBugCallbacksTest, StreamingReadTapResamplesFs8kToStream16k) {
    StreamClient client(harness_.MakeChannel(), MakeStreamConfig(16000), StreamCallbacks{});
    ASSERT_TRUE(client.Open(1000).ok());

    std::vector<std::int16_t> samples(160);
    std::iota(samples.begin(), samples.end(), static_cast<std::int16_t>(1));
    switch_frame_t frame = MakeFrame(&samples, 8000);
    osw::raii::fs::Mock().next_media_bug_read_frame = &frame;

    ReadCallbackCtx ctx;
    ctx.client = &client;
    ctx.fs_rate_hz = 8000;
    ctx.stream_rate_hz = 16000;

    EXPECT_EQ(OswStreamingReadTap(kFakeBug, &ctx, 1), SWITCH_TRUE);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));
    client.Close();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 320u);
    EXPECT_EQ(received[0].payload().size(), 320u * sizeof(std::int16_t));
    ASSERT_NE(ctx.resampler, nullptr);
    EXPECT_EQ(ctx.resampler->from_hz(), 8000);
    EXPECT_EQ(ctx.resampler->to_hz(), 16000);
}

TEST_F(MediaBugCallbacksTest, StreamingWriteReplaceResamplesStream16kToFs8k) {
    TtsPlayoutBuffer buffer(MakeBufferConfig(16000));
    std::vector<std::int16_t> service_samples(320, 1200);
    buffer.Push(AudioFrame(std::move(service_samples), 16000, 1, 0, 0));

    std::vector<std::int16_t> fs_samples(160, 0);
    switch_frame_t frame = MakeFrame(&fs_samples, 8000);
    osw::raii::fs::Mock().next_write_replace_frame = &frame;

    WriteCallbackCtx ctx;
    ctx.buffer = &buffer;
    ctx.stream_id = "tts-resample";
    ctx.fs_rate_hz = 8000;
    ctx.stream_rate_hz = 16000;

    EXPECT_EQ(OswStreamingWriteReplace(kFakeBug, &ctx, 3), SWITCH_TRUE);

    EXPECT_EQ(frame.samples, 160u);
    EXPECT_EQ(frame.datalen, 160u * sizeof(std::int16_t));
    EXPECT_EQ(frame.rate, 8000u);
    EXPECT_EQ(osw::raii::fs::Mock().media_bug_set_write_replace_frame_calls.load(), 1);
    ASSERT_NE(ctx.resampler, nullptr);
    EXPECT_EQ(ctx.resampler->from_hz(), 16000);
    EXPECT_EQ(ctx.resampler->to_hz(), 8000);
}

TEST_F(MediaBugCallbacksTest, BotReadTapResamplesFs8kToBotStream16k) {
    BotSession bot(MakeBotConfig(StartBotRequest::STT_LISTEN, 16000), harness_.MakeChannel());
    ASSERT_TRUE(bot.Open(1000).ok());

    std::vector<std::int16_t> samples(160, 77);
    switch_frame_t frame = MakeFrame(&samples, 8000);
    osw::raii::fs::Mock().next_media_bug_read_frame = &frame;

    BotReadTapCtx ctx;
    ctx.bot = &bot;
    ctx.channel_uuid = "chan-a";
    ctx.fs_rate_hz = 8000;
    ctx.stream_rate_hz = 16000;

    EXPECT_EQ(OswBotReadTap(kFakeBug, &ctx, 1), SWITCH_TRUE);
    ASSERT_TRUE(harness_.service().WaitForReceivedAudioCount(1, kWait));
    bot.Stop();

    const auto received = harness_.service().ReceivedAudioSnapshot();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].duration_samples(), 320u);
    EXPECT_EQ(received[0].payload().size(), 320u * sizeof(std::int16_t));
    EXPECT_EQ(received[0].channel_uuid(), "chan-a");
    ASSERT_NE(ctx.resampler, nullptr);
    EXPECT_EQ(ctx.resampler->from_hz(), 8000);
    EXPECT_EQ(ctx.resampler->to_hz(), 16000);
}

TEST_F(MediaBugCallbacksTest, BotWriteReplaceResamplesBot16kToFs8k) {
    harness_.service().SetServiceAudio({osw::test::MakeAudioProto(0, 320, 1, 2400)});

    BotSession bot(MakeBotConfig(StartBotRequest::TTS_BROADCAST, 16000), harness_.MakeChannel());
    ASSERT_TRUE(bot.Open(1000).ok());
    for (int i = 0; i < 100 && bot.FramesReceivedFromUpstream() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(bot.FramesReceivedFromUpstream(), 1u);

    std::vector<std::int16_t> fs_samples(160, 0);
    switch_frame_t frame = MakeFrame(&fs_samples, 8000);
    osw::raii::fs::Mock().next_write_replace_frame = &frame;

    BotWriteReplaceCtx ctx;
    ctx.bot = &bot;
    ctx.channel_uuid = "chan-a";
    ctx.fs_rate_hz = 8000;

    EXPECT_EQ(OswBotWriteReplace(kFakeBug, &ctx, 3), SWITCH_TRUE);
    bot.Stop();

    EXPECT_EQ(frame.samples, 160u);
    EXPECT_EQ(frame.datalen, 160u * sizeof(std::int16_t));
    EXPECT_EQ(frame.rate, 8000u);
    EXPECT_EQ(osw::raii::fs::Mock().media_bug_set_write_replace_frame_calls.load(), 1);
    ASSERT_NE(ctx.resampler, nullptr);
    EXPECT_EQ(ctx.resampler->from_hz(), 16000);
    EXPECT_EQ(ctx.resampler->to_hz(), 8000);
}

}  // namespace
