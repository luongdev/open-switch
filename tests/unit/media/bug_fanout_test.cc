/*
 * tests/unit/media/bug_fanout_test.cc
 *
 * Pure unit tests for osw::media::BugFanout.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/bug_fanout.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "osw/media/audio_frame.h"

namespace {

using osw::media::AudioFrame;
using osw::media::BugFanout;

constexpr std::uint32_t kRate = 16000;
constexpr std::uint32_t kSamples20ms = kRate / 50;
constexpr const char* kTarget1 = "target-1";
constexpr const char* kTarget2 = "target-2";

AudioFrame MakeFrame(std::int16_t fill_value, std::uint64_t seq = 0) {
    return AudioFrame(std::vector<std::int16_t>(kSamples20ms, fill_value),
                      kRate,
                      /*channels=*/1,
                      seq,
                      seq * kSamples20ms);
}

BugFanout MakeFanout(BugFanout::Mode mode,
                     std::uint32_t capacity_frames,
                     std::vector<std::string> write_subset = {}) {
    BugFanout::Config cfg;
    cfg.mode = mode;
    cfg.target_uuids = {kTarget1, kTarget2};
    cfg.write_subset_uuids = std::move(write_subset);
    cfg.capacity_frames = capacity_frames;
    return BugFanout(std::move(cfg));
}

TEST(BugFanoutTest, BroadcastMode_PushDeliversToAll) {
    auto fanout = MakeFanout(BugFanout::Mode::kBroadcast, 4);

    EXPECT_EQ(fanout.Push(MakeFrame(111)), 0u);

    EXPECT_EQ(fanout.QueueDepth(kTarget1), 1u);
    EXPECT_EQ(fanout.QueueDepth(kTarget2), 1u);

    const auto frame1 = fanout.Pop(kTarget1);
    const auto frame2 = fanout.Pop(kTarget2);

    ASSERT_TRUE(frame1.has_value());
    ASSERT_TRUE(frame2.has_value());
    EXPECT_EQ(frame1->sample_count(), kSamples20ms);
    EXPECT_EQ(frame2->sample_count(), kSamples20ms);
    EXPECT_EQ(frame1->data()[0], 111);
    EXPECT_EQ(frame2->data()[0], 111);
}

TEST(BugFanoutTest, WhisperMode_PushDeliversToSubsetOnly) {
    auto fanout = MakeFanout(BugFanout::Mode::kWhisper, 4, {kTarget2});

    EXPECT_EQ(fanout.Push(MakeFrame(222)), 0u);

    EXPECT_EQ(fanout.QueueDepth(kTarget1), 0u);
    EXPECT_EQ(fanout.QueueDepth(kTarget2), 1u);
    EXPECT_FALSE(fanout.Pop(kTarget1).has_value());

    const auto frame = fanout.Pop(kTarget2);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->data()[0], 222);
}

TEST(BugFanoutTest, Capacity_DropsOldestOnOverflow) {
    BugFanout::Config cfg;
    cfg.mode = BugFanout::Mode::kBroadcast;
    cfg.target_uuids = {kTarget1};
    cfg.capacity_frames = 4;
    BugFanout fanout(std::move(cfg));

    for (std::int16_t value = 1; value <= 4; ++value) {
        EXPECT_EQ(fanout.Push(MakeFrame(value)), 0u);
    }

    EXPECT_EQ(fanout.Push(MakeFrame(5)), 1u);
    EXPECT_EQ(fanout.QueueDepth(kTarget1), 4u);
    EXPECT_EQ(fanout.TotalDropped(), 1u);

    const auto first = fanout.Pop(kTarget1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->data()[0], 2);
}

TEST(BugFanoutTest, HalfClose_NoMorePushes) {
    auto fanout = MakeFanout(BugFanout::Mode::kBroadcast, 4);

    fanout.HalfClose();

    EXPECT_EQ(fanout.Push(MakeFrame(1)), 0u);
    EXPECT_EQ(fanout.TotalDropped(), 0u);
    EXPECT_EQ(fanout.QueueDepth(kTarget1), 0u);
    EXPECT_EQ(fanout.QueueDepth(kTarget2), 0u);
}

TEST(BugFanoutTest, HalfClose_PopStillDrains) {
    BugFanout::Config cfg;
    cfg.mode = BugFanout::Mode::kBroadcast;
    cfg.target_uuids = {kTarget1};
    cfg.capacity_frames = 4;
    BugFanout fanout(std::move(cfg));

    EXPECT_EQ(fanout.Push(MakeFrame(1)), 0u);
    EXPECT_EQ(fanout.Push(MakeFrame(2)), 0u);
    EXPECT_EQ(fanout.Push(MakeFrame(3)), 0u);

    fanout.HalfClose();

    for (std::int16_t expected = 1; expected <= 3; ++expected) {
        const auto frame = fanout.Pop(kTarget1);
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->data()[0], expected);
    }
    EXPECT_FALSE(fanout.Pop(kTarget1).has_value());
}

TEST(BugFanoutTest, EmptyQueue_PopReturnsNullopt) {
    auto fanout = MakeFanout(BugFanout::Mode::kBroadcast, 4);

    EXPECT_FALSE(fanout.Pop(kTarget1).has_value());
    EXPECT_FALSE(fanout.Pop("unknown-target").has_value());
}

TEST(BugFanoutTest, ConcurrentPushPop_TsanClean) {
    BugFanout::Config cfg;
    cfg.mode = BugFanout::Mode::kBroadcast;
    cfg.target_uuids = {kTarget1};
    cfg.capacity_frames = 1000;
    BugFanout fanout(std::move(cfg));

    std::atomic<bool> producer_done{false};
    std::atomic<std::uint32_t> popped{0};

    std::thread producer([&fanout, &producer_done] {
        for (std::uint64_t seq = 0; seq < 1000; ++seq) {
            fanout.Push(MakeFrame(static_cast<std::int16_t>(seq), seq));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&fanout, &producer_done, &popped] {
        while (!producer_done.load(std::memory_order_acquire) || fanout.QueueDepth(kTarget1) > 0) {
            if (fanout.Pop(kTarget1).has_value()) {
                popped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(popped.load(std::memory_order_relaxed), 1000u);
    EXPECT_EQ(fanout.TotalDropped(), 0u);
    EXPECT_EQ(fanout.QueueDepth(kTarget1), 0u);
}

}  // namespace
