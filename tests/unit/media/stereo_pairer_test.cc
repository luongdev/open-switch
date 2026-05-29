/*
 * tests/unit/media/stereo_pairer_test.cc
 *
 * Unit tests for osw::media::StereoFramePairer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/stereo_pairer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using osw::media::StereoFramePairer;

constexpr std::uint32_t kRate = 8000;
constexpr std::uint64_t kSamples5ms = kRate * 5 / 1000;
constexpr std::uint64_t kSamples20ms = kRate / 50;

StereoFramePairer MakePairer(std::uint32_t warn_ms = 5, std::uint32_t timeout_ms = 25) {
    StereoFramePairer::Config cfg;
    cfg.sample_rate_hz = kRate;
    cfg.desync_warn_ms = warn_ms;
    cfg.desync_timeout_ms = timeout_ms;
    return StereoFramePairer(cfg);
}

std::int16_t ReadLeSample(const std::vector<std::uint8_t>& bytes, std::size_t sample_index) {
    const std::size_t offset = sample_index * sizeof(std::int16_t);
    const auto value = static_cast<std::uint16_t>(bytes[offset]) |
                       static_cast<std::uint16_t>(bytes[offset + 1] << 8u);
    return static_cast<std::int16_t>(value);
}

TEST(StereoFramePairerTest, PairSimultaneousLR_Interleaves) {
    auto pairer = MakePairer();

    const std::int16_t left[] = {1, -2, 300};
    const std::int16_t right[] = {10, 20, -30};

    EXPECT_FALSE(pairer.PushLeft(0, left).has_value());
    auto paired = pairer.PushRight(0, right);

    ASSERT_TRUE(paired.has_value());
    EXPECT_EQ(paired->sample_rate_hz, kRate);
    EXPECT_EQ(paired->samples_per_channel, 3u);
    EXPECT_EQ(paired->seq, 0u);
    ASSERT_EQ(paired->interleaved.size(), 12u);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 0), 1);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 1), 10);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 2), -2);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 3), 20);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 4), 300);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 5), -30);
    EXPECT_EQ(pairer.PairedCount(), 1u);
    EXPECT_EQ(pairer.DesyncCount(), 0u);
    EXPECT_EQ(pairer.WarnCount(), 0u);
}

TEST(StereoFramePairerTest, Pair5msDesync_WarnsWithoutSilenceFill) {
    auto pairer = MakePairer();

    const std::int16_t left[] = {111};
    const std::int16_t right[] = {222};

    EXPECT_FALSE(pairer.PushLeft(0, left).has_value());
    auto paired = pairer.PushRight(kSamples5ms, right);

    ASSERT_TRUE(paired.has_value());
    EXPECT_EQ(ReadLeSample(paired->interleaved, 0), 111);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 1), 222);
    EXPECT_EQ(pairer.PairedCount(), 1u);
    EXPECT_EQ(pairer.DesyncCount(), 0u);
    EXPECT_EQ(pairer.WarnCount(), 1u);
}

TEST(StereoFramePairerTest, PairTimeoutFlush_SilenceFillFromTick) {
    auto pairer = MakePairer(/*warn_ms=*/5, /*timeout_ms=*/1);

    const std::int16_t left[] = {7, 8, 9};
    EXPECT_FALSE(pairer.PushLeft(0, left).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    auto paired = pairer.Tick();

    ASSERT_TRUE(paired.has_value());
    EXPECT_EQ(paired->samples_per_channel, 3u);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 0), 7);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 1), 0);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 2), 8);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 3), 0);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 4), 9);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 5), 0);
    EXPECT_EQ(pairer.PairedCount(), 1u);
    EXPECT_EQ(pairer.DesyncCount(), 1u);
}

TEST(StereoFramePairerTest, RingDropOldest_FrameLoss) {
    auto pairer = MakePairer(/*warn_ms=*/5, /*timeout_ms=*/1000);

    for (std::int16_t i = 1; i <= 17; ++i) {
        const std::int16_t sample[] = {i};
        EXPECT_FALSE(
            pairer.PushLeft(static_cast<std::uint64_t>(i - 1) * kSamples20ms, sample).has_value());
    }

    EXPECT_EQ(pairer.DroppedCount(), 1u);

    const std::int16_t right[] = {20};
    auto paired = pairer.PushRight(kSamples20ms, right);

    ASSERT_TRUE(paired.has_value());
    EXPECT_EQ(ReadLeSample(paired->interleaved, 0), 2);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 1), 20);
}

TEST(StereoFramePairerTest, MutexSafeUnderConcurrentPush) {
    auto pairer = MakePairer();
    std::atomic<int> turn{0};
    std::atomic<std::uint32_t> returned_pairs{0};

    std::thread left_thread([&] {
        for (std::uint64_t i = 0; i < 1000; ++i) {
            while (turn.load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
            const std::int16_t sample[] = {static_cast<std::int16_t>(i)};
            if (pairer.PushLeft(i * kSamples20ms, sample).has_value()) {
                returned_pairs.fetch_add(1, std::memory_order_relaxed);
            }
            turn.store(1, std::memory_order_release);
        }
    });

    std::thread right_thread([&] {
        for (std::uint64_t i = 0; i < 1000; ++i) {
            while (turn.load(std::memory_order_acquire) != 1) {
                std::this_thread::yield();
            }
            const std::int16_t sample[] = {static_cast<std::int16_t>(i)};
            if (pairer.PushRight(i * kSamples20ms, sample).has_value()) {
                returned_pairs.fetch_add(1, std::memory_order_relaxed);
            }
            turn.store(0, std::memory_order_release);
        }
    });

    left_thread.join();
    right_thread.join();

    EXPECT_EQ(returned_pairs.load(std::memory_order_relaxed), 1000u);
    EXPECT_EQ(pairer.PairedCount(), 1000u);
    EXPECT_EQ(pairer.DesyncCount(), 0u);
    EXPECT_EQ(pairer.DroppedCount(), 0u);
}

TEST(StereoFramePairerTest, SilencePayloadFormat_RightOnlyFillsLeftWithZeroBytes) {
    auto pairer = MakePairer(/*warn_ms=*/5, /*timeout_ms=*/1);

    const std::int16_t right[] = {-123, 456};
    EXPECT_FALSE(pairer.PushRight(0, right).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    auto paired = pairer.Tick();

    ASSERT_TRUE(paired.has_value());
    ASSERT_EQ(paired->interleaved.size(), 8u);
    EXPECT_EQ(paired->interleaved[0], 0u);
    EXPECT_EQ(paired->interleaved[1], 0u);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 1), -123);
    EXPECT_EQ(paired->interleaved[4], 0u);
    EXPECT_EQ(paired->interleaved[5], 0u);
    EXPECT_EQ(ReadLeSample(paired->interleaved, 3), 456);
    EXPECT_EQ(pairer.DesyncCount(), 1u);
}

}  // namespace
