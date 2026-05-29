/*
 * tests/unit/media/audio_frame_test.cc
 *
 * Unit tests for osw::media::AudioFrame.
 *
 * Scenarios (from W6 Track B spec):
 *   F1 — Construct + accessors: round-trip values intact.
 *   F2 — duration_samples for 160-sample mono frame: 160.
 *   F3 — duration_samples for 320-sample stereo (channels=2): 160.
 *   F4 — duration_ms for 160 samples @ 8000 Hz mono: 20.
 *   F5 — ToProto + FromProto round trip: bytes match; seq/ts preserved.
 *   F6 — FromProto with mismatched payload size: nullopt.
 *   F7 — Move-construct: sample buffer moved, source empty.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/audio_frame.h"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/media/v1/media.pb.h"

namespace {

using osw::media::AudioFrame;

// Helper: build a mono 20ms frame at 8 kHz (160 samples, ramp 0..159).
static AudioFrame MakeMono8k(std::uint64_t seq = 0, std::uint64_t ts = 0) {
    std::vector<std::int16_t> samples(160);
    for (int i = 0; i < 160; ++i) {
        samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i);
    }
    return AudioFrame(std::move(samples), 8000, 1, seq, ts);
}

// ---------------------------------------------------------------------------
// F1 — Construct + accessors
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F1_ConstructAndAccessors) {
    auto f = MakeMono8k(42, 12345);
    EXPECT_EQ(f.sample_count(), 160u);
    EXPECT_EQ(f.sample_rate_hz(), 8000u);
    EXPECT_EQ(f.channels(), 1u);
    EXPECT_EQ(f.seq(), 42u);
    EXPECT_EQ(f.timestamp_samples(), 12345u);
    EXPECT_NE(f.data(), nullptr);
    // Spot-check a sample value.
    EXPECT_EQ(f.data()[0], 0);
    EXPECT_EQ(f.data()[159], 159);
}

// ---------------------------------------------------------------------------
// F2 — duration_samples for 160-sample mono frame
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F2_DurationSamplesMono) {
    auto f = MakeMono8k();
    EXPECT_EQ(f.duration_samples(), 160u);
}

// ---------------------------------------------------------------------------
// F3 — duration_samples for 320-sample stereo (channels=2)
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F3_DurationSamplesStereo) {
    std::vector<std::int16_t> samples(320, 0);  // 160 per channel × 2 channels
    AudioFrame f(std::move(samples), 16000, 2, 0, 0);
    EXPECT_EQ(f.duration_samples(), 160u);
}

// ---------------------------------------------------------------------------
// F4 — duration_ms for 160 samples @ 8000 Hz mono
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F4_DurationMs8kMono) {
    auto f = MakeMono8k();
    EXPECT_EQ(f.duration_ms(), 20u);
}

// ---------------------------------------------------------------------------
// F5 — ToProto + FromProto round trip
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F5_ProtoRoundTrip) {
    std::vector<std::int16_t> samples(160, 7);
    AudioFrame original(
        std::move(samples),
        8000,
        1,
        7,
        999,
        static_cast<std::uint32_t>(open_switch::media::v1::AudioFrame::BOTH_INTERLEAVED),
        "chan-a");

    open_switch::media::v1::AudioFrame proto;
    original.ToProto(&proto);

    EXPECT_EQ(proto.seq(), 7u);
    EXPECT_EQ(proto.timestamp_samples(), 999u);
    EXPECT_EQ(proto.duration_samples(), 160u);
    EXPECT_EQ(proto.channel(), open_switch::media::v1::AudioFrame::BOTH_INTERLEAVED);
    EXPECT_EQ(proto.channel_uuid(), "chan-a");
    // Payload size = 160 samples × 1 channel × 2 bytes = 320 bytes.
    EXPECT_EQ(proto.payload().size(), 320u);

    auto recovered = AudioFrame::FromProto(proto, 8000, 1);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->seq(), 7u);
    EXPECT_EQ(recovered->timestamp_samples(), 999u);
    EXPECT_EQ(recovered->sample_count(), 160u);
    EXPECT_EQ(recovered->sample_rate_hz(), 8000u);
    EXPECT_EQ(recovered->channels(), 1u);
    EXPECT_EQ(recovered->channel(),
              static_cast<std::uint32_t>(open_switch::media::v1::AudioFrame::BOTH_INTERLEAVED));
    EXPECT_EQ(recovered->channel_uuid(), "chan-a");

    // Sample-level equality.
    for (std::size_t i = 0; i < 160; ++i) {
        EXPECT_EQ(recovered->data()[i], original.data()[i]) << "Sample mismatch at index " << i;
    }
}

// ---------------------------------------------------------------------------
// F6 — FromProto with mismatched payload size
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F6_FromProtoPayloadMismatch) {
    open_switch::media::v1::AudioFrame proto;
    proto.set_seq(0);
    proto.set_timestamp_samples(0);
    proto.set_duration_samples(160);
    // Write only 100 bytes instead of the expected 320.
    proto.set_payload(std::string(100, '\0'));

    auto result = AudioFrame::FromProto(proto, 8000, 1);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// F7 — Move-construct: sample buffer moved, source empty
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, F7_MoveConstruct) {
    auto src = MakeMono8k(1, 2);
    EXPECT_EQ(src.sample_count(), 160u);

    AudioFrame dst(std::move(src));
    EXPECT_EQ(dst.sample_count(), 160u);
    EXPECT_EQ(dst.seq(), 1u);
    EXPECT_EQ(dst.timestamp_samples(), 2u);
    // After move, src's sample_count should be 0 (vector was moved from).
    EXPECT_EQ(src.sample_count(), 0u);  // NOLINT(bugprone-use-after-move)
}

// ---------------------------------------------------------------------------
// Extra: default-constructed frame is safe
// ---------------------------------------------------------------------------
TEST(AudioFrameTest, DefaultConstructedIsEmpty) {
    AudioFrame f;
    EXPECT_EQ(f.sample_count(), 0u);
    EXPECT_EQ(f.sample_rate_hz(), 0u);
    EXPECT_EQ(f.duration_samples(), 0u);
    EXPECT_EQ(f.duration_ms(), 0u);
}

}  // namespace
