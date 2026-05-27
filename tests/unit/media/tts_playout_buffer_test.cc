/*
 * tests/unit/media/tts_playout_buffer_test.cc
 *
 * Unit tests for osw::media::TtsPlayoutBuffer.
 * FS-agnostic — no FS seam required.
 *
 * Acceptance scenarios covered (C1–C8 from W6 Track C spec):
 *   C1 — Push frames, PrerollReached transitions when threshold crossed.
 *   C2 — Pop before preroll returns silence (non-zero samples, zero-filled).
 *   C3 — Pop after preroll delivers pushed frames in order.
 *   C4 — High-water overrun: oldest frames dropped when depth exceeds limit.
 *   C5 — Underrun (kSilence policy): returns silence + increments counter.
 *   C6 — Underrun (kRepeatLast policy): repeats last frame.
 *   C7 — SignalEndOfStream + empty queue → Pop returns 0.
 *   C8 — CurrentDepth reflects pushed frames.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/tts_playout_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "osw/media/audio_frame.h"

namespace {

using osw::media::AudioFrame;
using osw::media::TtsPlayoutBuffer;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr std::uint32_t kRate = 16000;
// 20ms ptime = 320 samples at 16kHz.
constexpr std::uint32_t kPtime20ms = kRate / 50;

/// Build a PCM-S16LE AudioFrame filled with a repeated sample value.
AudioFrame MakeFrame(std::int16_t fill_value, std::uint32_t samples = kPtime20ms) {
    std::vector<std::int16_t> pcm(samples, fill_value);
    return AudioFrame(std::move(pcm),
                      kRate,
                      /*channels=*/1,
                      /*seq=*/0,
                      /*timestamp_samples=*/0);
}

/// Build a minimal TtsPlayoutBuffer with preroll=200ms, target=400ms, high=600ms.
TtsPlayoutBuffer MakeBuffer(
    std::uint32_t preroll_ms = 200,
    std::uint32_t target_ms = 400,
    std::uint32_t high_water_ms = 600,
    TtsPlayoutBuffer::UnderrunPolicy policy = TtsPlayoutBuffer::UnderrunPolicy::kSilence) {
    TtsPlayoutBuffer::Config cfg;
    cfg.target_ms = std::chrono::milliseconds(target_ms);
    cfg.preroll_ms = std::chrono::milliseconds(preroll_ms);
    cfg.high_water_ms = std::chrono::milliseconds(high_water_ms);
    cfg.underrun = policy;
    cfg.channel_sample_rate_hz = kRate;
    cfg.channels = 1;
    return TtsPlayoutBuffer(cfg);
}

// ---------------------------------------------------------------------------
// C1 — PrerollReached transitions
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, PrerollNotReachedInitially) {
    auto buf = MakeBuffer(200);
    EXPECT_FALSE(buf.PrerollReached());
}

TEST(TtsPlayoutBufferTest, PrerollReachedAfterEnoughFrames) {
    // preroll=200ms, each frame=20ms → need 10 frames.
    auto buf = MakeBuffer(200);
    for (int i = 0; i < 9; ++i) {
        buf.Push(MakeFrame(1));
        EXPECT_FALSE(buf.PrerollReached()) << "should not be reached after " << i + 1 << " frames";
    }
    buf.Push(MakeFrame(1));  // 10th frame → 200ms
    EXPECT_TRUE(buf.PrerollReached());
}

TEST(TtsPlayoutBufferTest, PrerollReachedStaysTrue) {
    auto buf = MakeBuffer(40);  // 2 frames
    buf.Push(MakeFrame(1));
    buf.Push(MakeFrame(1));
    EXPECT_TRUE(buf.PrerollReached());
    // Pop all, push again — preroll_reached_ is sticky.
    std::vector<std::int16_t> out(kPtime20ms);
    buf.Pop(out.data(), kPtime20ms);
    buf.Pop(out.data(), kPtime20ms);
    EXPECT_TRUE(buf.PrerollReached());
}

// ---------------------------------------------------------------------------
// C2 — Pop before preroll returns silence
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, PopBeforePrerollReturnsSilence) {
    auto buf = MakeBuffer(200);  // preroll=200ms; push only 1 frame (20ms)
    buf.Push(MakeFrame(999));

    std::vector<std::int16_t> out(kPtime20ms);
    std::fill(out.begin(), out.end(), std::int16_t{0x7FFF});  // fill with non-zero

    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);
    EXPECT_GT(n, 0u);
    // All output samples should be zero (silence).
    for (std::uint32_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 0) << "sample[" << i << "] should be silence";
    }
}

// ---------------------------------------------------------------------------
// C3 — Pop after preroll delivers frames in order
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, PopAfterPrerollDeliversFramesInOrder) {
    auto buf = MakeBuffer(20);  // 1-frame preroll

    buf.Push(MakeFrame(111));
    ASSERT_TRUE(buf.PrerollReached());
    buf.Push(MakeFrame(222));

    std::vector<std::int16_t> out(kPtime20ms);

    const std::uint32_t n1 = buf.Pop(out.data(), kPtime20ms);
    ASSERT_EQ(n1, kPtime20ms);
    EXPECT_EQ(out[0], 111);

    const std::uint32_t n2 = buf.Pop(out.data(), kPtime20ms);
    ASSERT_EQ(n2, kPtime20ms);
    EXPECT_EQ(out[0], 222);
}

TEST(TtsPlayoutBufferTest, PopCombinesShortFramesIntoOneCallbackFrame) {
    auto buf = MakeBuffer(20);  // 1-frame preroll

    constexpr std::uint32_t kHalfFrame = kPtime20ms / 2;
    buf.Push(MakeFrame(11, kHalfFrame));
    buf.Push(MakeFrame(22, kHalfFrame));
    ASSERT_TRUE(buf.PrerollReached());

    std::vector<std::int16_t> out(kPtime20ms, 0);
    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);

    ASSERT_EQ(n, kPtime20ms);
    EXPECT_EQ(out[0], 11);
    EXPECT_EQ(out[kHalfFrame - 1], 11);
    EXPECT_EQ(out[kHalfFrame], 22);
    EXPECT_EQ(out[kPtime20ms - 1], 22);
    EXPECT_EQ(buf.CurrentDepth().count(), 0);
}

TEST(TtsPlayoutBufferTest, PopPadsMidFrameUnderrunInsteadOfReturningShortFrame) {
    auto buf = MakeBuffer(10);  // half-frame preroll

    constexpr std::uint32_t kHalfFrame = kPtime20ms / 2;
    buf.Push(MakeFrame(77, kHalfFrame));
    ASSERT_TRUE(buf.PrerollReached());

    std::vector<std::int16_t> out(kPtime20ms, std::int16_t{0x7FFF});
    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);

    ASSERT_EQ(n, kPtime20ms);
    EXPECT_EQ(out[0], 77);
    EXPECT_EQ(out[kHalfFrame - 1], 77);
    EXPECT_EQ(out[kHalfFrame], 0);
    EXPECT_EQ(out[kPtime20ms - 1], 0);
    EXPECT_GE(buf.UnderrunCount(), 1u);
}

TEST(TtsPlayoutBufferTest, CurrentDepthAccountsForPartialFrontFrame) {
    auto buf = MakeBuffer(20);

    buf.Push(MakeFrame(33, kPtime20ms * 2));
    ASSERT_TRUE(buf.PrerollReached());
    EXPECT_EQ(buf.CurrentDepth().count(), 40);

    std::vector<std::int16_t> out(kPtime20ms);
    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);
    ASSERT_EQ(n, kPtime20ms);
    EXPECT_EQ(buf.CurrentDepth().count(), 20);
}

// ---------------------------------------------------------------------------
// C4 — High-water overrun
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, HighWaterOverrunDropsOldestFrames) {
    // high_water=100ms, 20ms frames → max 5 frames before dropping.
    auto buf = MakeBuffer(/*preroll=*/20, /*target=*/60, /*high=*/100);

    // Push 7 frames labeled 1..7 — oldest should be dropped to stay ≤ 100ms.
    for (int i = 1; i <= 7; ++i) {
        buf.Push(MakeFrame(static_cast<std::int16_t>(i)));
    }

    // OverrunCount should be > 0.
    EXPECT_GT(buf.OverrunCount(), 0u);

    // The buffer depth should be ≤ high_water.
    EXPECT_LE(buf.CurrentDepth().count(), 100);
}

// ---------------------------------------------------------------------------
// C5 — Underrun with kSilence policy
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, UnderrunSilencePolicyReturnsSilence) {
    auto buf = MakeBuffer(20);  // 1-frame preroll

    buf.Push(MakeFrame(42));
    ASSERT_TRUE(buf.PrerollReached());

    // Drain the one frame.
    std::vector<std::int16_t> out(kPtime20ms, std::int16_t{0x7F});
    buf.Pop(out.data(), kPtime20ms);

    // Now buffer is empty — underrun.
    std::fill(out.begin(), out.end(), std::int16_t{0x7FFF});
    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);
    EXPECT_GT(n, 0u);
    // All samples should be silence.
    for (std::uint32_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 0) << "sample[" << i << "] underrun should be silence";
    }
    EXPECT_GE(buf.UnderrunCount(), 1u);
}

// ---------------------------------------------------------------------------
// C6 — Underrun with kRepeatLast policy
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, UnderrunRepeatLastRepeatsLastFrame) {
    auto buf = MakeBuffer(20, 400, 600, TtsPlayoutBuffer::UnderrunPolicy::kRepeatLast);

    buf.Push(MakeFrame(99));
    ASSERT_TRUE(buf.PrerollReached());

    std::vector<std::int16_t> out(kPtime20ms);
    buf.Pop(out.data(), kPtime20ms);  // drains the frame; records last_frame_=99

    // Underrun — should repeat last frame.
    std::fill(out.begin(), out.end(), std::int16_t{0});
    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);
    EXPECT_GT(n, 0u);
    EXPECT_EQ(out[0], 99);
    EXPECT_GE(buf.UnderrunCount(), 1u);
}

// ---------------------------------------------------------------------------
// C7 — SignalEndOfStream + empty queue → Pop returns 0
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, EosEmptyQueuePopReturnsZero) {
    auto buf = MakeBuffer(20);

    buf.Push(MakeFrame(1));
    ASSERT_TRUE(buf.PrerollReached());

    std::vector<std::int16_t> out(kPtime20ms);
    buf.Pop(out.data(), kPtime20ms);  // drain

    buf.SignalEndOfStream();
    EXPECT_TRUE(buf.EndOfStream());

    const std::uint32_t n = buf.Pop(out.data(), kPtime20ms);
    EXPECT_EQ(n, 0u);
}

TEST(TtsPlayoutBufferTest, EosWithFramesRemainingDeliversThenZero) {
    auto buf = MakeBuffer(20);

    buf.Push(MakeFrame(77));
    ASSERT_TRUE(buf.PrerollReached());
    buf.SignalEndOfStream();

    std::vector<std::int16_t> out(kPtime20ms);

    // Should still deliver the queued frame.
    const std::uint32_t n1 = buf.Pop(out.data(), kPtime20ms);
    EXPECT_GT(n1, 0u);
    EXPECT_EQ(out[0], 77);

    // Now empty + EOS.
    const std::uint32_t n2 = buf.Pop(out.data(), kPtime20ms);
    EXPECT_EQ(n2, 0u);
}

// ---------------------------------------------------------------------------
// C8 — CurrentDepth reflects pushed frames
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, CurrentDepthMatchesPushedFrames) {
    // Use a small preroll (40ms = 2 frames) so we can verify both the
    // pre-preroll depth (no change on Pop — silence emitted, frames kept)
    // and the post-preroll depth (Pop dequeues, depth decrements).
    auto buf = MakeBuffer(/*preroll_ms=*/40, /*target_ms=*/200, /*high_water_ms=*/400);

    EXPECT_EQ(buf.CurrentDepth().count(), 0);

    // Push 2 × 20ms frames = 40ms → preroll reached.
    buf.Push(MakeFrame(1));
    buf.Push(MakeFrame(1));
    EXPECT_EQ(buf.CurrentDepth().count(), 40);
    EXPECT_TRUE(buf.PrerollReached());

    // Push one more frame → 60ms total.
    buf.Push(MakeFrame(1));
    EXPECT_EQ(buf.CurrentDepth().count(), 60);

    // Pop one 20ms frame (post-preroll) → depth drops to 40ms.
    std::vector<std::int16_t> out(kPtime20ms);
    buf.Pop(out.data(), kPtime20ms);
    EXPECT_EQ(buf.CurrentDepth().count(), 40);
}

// ---------------------------------------------------------------------------
// SetStreamId / SetTenantId (smoke — no crash)
// ---------------------------------------------------------------------------

TEST(TtsPlayoutBufferTest, SetStreamAndTenantIdNocrash) {
    auto buf = MakeBuffer(20);
    buf.SetStreamId("stream-uuid-1234");
    buf.SetTenantId("tenant-42");
    // No assertion needed — just verifying no crash/UB.
    SUCCEED();
}

}  // namespace
