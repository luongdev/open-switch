/*
 * tests/unit/observability/health_test.cc
 *
 * Unit tests for osw::Health.
 *
 * Covered:
 *   - Default status is kServing.
 *   - SetVersions populates the snapshot string fields.
 *   - SetStatus changes the GetStatus + GetSnapshot status.
 *   - All counter setters propagate to Snapshot.
 *   - Snapshot copies are independent.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/health.h"

#include <gtest/gtest.h>

namespace {

TEST(HealthTest, DefaultStatusIsServing) {
    osw::Health h;
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kServing);
    EXPECT_EQ(h.GetSnapshot().status, osw::Health::Status::kServing);
}

TEST(HealthTest, SetVersionsPopulatesSnapshot) {
    osw::Health h;
    h.SetVersions("0.1.0", "FreeSWITCH 1.10.12-dev");
    auto s = h.GetSnapshot();
    EXPECT_EQ(s.module_version, "0.1.0");
    EXPECT_EQ(s.freeswitch_version, "FreeSWITCH 1.10.12-dev");
}

TEST(HealthTest, SetStatusFlipToDraining) {
    osw::Health h;
    h.SetStatus(osw::Health::Status::kDraining);
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kDraining);
    EXPECT_EQ(h.GetSnapshot().status, osw::Health::Status::kDraining);
}

TEST(HealthTest, SetStatusIsIdempotent) {
    osw::Health h;
    h.SetStatus(osw::Health::Status::kDraining);
    h.SetStatus(osw::Health::Status::kDraining);
    EXPECT_EQ(h.GetStatus(), osw::Health::Status::kDraining);
}

TEST(HealthTest, CountersPropagateToSnapshot) {
    osw::Health h;
    h.SetActiveChannels(42);
    h.SetActiveMediaBugs(7);
    h.SetEventsEmittedTotal(123456);
    h.SetSubscriberCount(3);
    h.SetTier1RingFillPct(10);
    h.SetTier2RingFillPct(20);
    h.SetTier3RingFillPct(30);
    h.SetTier1DroppedTotal(11);
    h.SetTier2DroppedTotal(22);
    h.SetTier3DroppedTotal(33);

    auto s = h.GetSnapshot();
    EXPECT_EQ(s.active_channels, 42u);
    EXPECT_EQ(s.active_media_bugs, 7u);
    EXPECT_EQ(s.events_emitted_total, 123456u);
    EXPECT_EQ(s.subscriber_count, 3u);
    EXPECT_EQ(s.tier1_ring_fill_pct, 10u);
    EXPECT_EQ(s.tier2_ring_fill_pct, 20u);
    EXPECT_EQ(s.tier3_ring_fill_pct, 30u);
    EXPECT_EQ(s.tier1_dropped_total, 11u);
    EXPECT_EQ(s.tier2_dropped_total, 22u);
    EXPECT_EQ(s.tier3_dropped_total, 33u);
}

TEST(HealthTest, SnapshotsAreIndependent) {
    osw::Health h;
    h.SetActiveChannels(1);
    auto s1 = h.GetSnapshot();
    h.SetActiveChannels(999);
    auto s2 = h.GetSnapshot();
    EXPECT_EQ(s1.active_channels, 1u);
    EXPECT_EQ(s2.active_channels, 999u);
}

}  // namespace
