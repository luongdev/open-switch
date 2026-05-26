/*
 * tests/unit/core/lifecycle_test.cc
 *
 * Tests for osw::Lifecycle status transitions. Covers:
 *   - default state kLoaded
 *   - TransitionToServing flips state + Health to Serving
 *   - SignalDrain flips state to Draining + Health to Draining
 *   - SignalDrain is idempotent
 *   - MarkStopped flips to kStopped + Health to NotServing
 *   - Reverse transitions blocked (Stopped -> anything)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/lifecycle.h"

#include <gtest/gtest.h>

#include "osw/observability/health.h"

namespace {

TEST(LifecycleTest, DefaultStateIsLoaded) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kLoaded);
}

TEST(LifecycleTest, TransitionToServingFlipsHealth) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    lc.TransitionToServing();
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kServing);
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kServing);
}

TEST(LifecycleTest, SignalDrainFromServingFlipsBoth) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    lc.TransitionToServing();
    const bool first = lc.SignalDrain();
    EXPECT_TRUE(first);
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kDraining);
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kDraining);
}

TEST(LifecycleTest, SignalDrainIsIdempotent) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    lc.TransitionToServing();
    EXPECT_TRUE(lc.SignalDrain());
    EXPECT_FALSE(lc.SignalDrain());
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kDraining);
}

TEST(LifecycleTest, SignalDrainFromLoadedIsAllowed) {
    // Load may fail mid-way; the shutdown path still calls SignalDrain
    // even though we never got to Serving. Should succeed.
    osw::Health h;
    osw::Lifecycle lc(&h);
    EXPECT_TRUE(lc.SignalDrain());
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kDraining);
}

TEST(LifecycleTest, MarkStoppedFlipsHealthToNotServing) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    lc.TransitionToServing();
    lc.SignalDrain();
    lc.MarkStopped();
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kStopped);
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kNotServing);
}

TEST(LifecycleTest, SignalDrainAfterStoppedReturnsFalse) {
    osw::Health h;
    osw::Lifecycle lc(&h);
    lc.MarkStopped();
    EXPECT_FALSE(lc.SignalDrain());
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kStopped);
}

TEST(LifecycleTest, NullHealthIsTolerated) {
    osw::Lifecycle lc(nullptr);
    lc.TransitionToServing();
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kServing);
    lc.SignalDrain();
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kDraining);
    lc.MarkStopped();
    EXPECT_EQ(lc.GetState(), osw::Lifecycle::State::kStopped);
}

}  // namespace
