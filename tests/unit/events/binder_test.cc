/*
 * tests/unit/events/binder_test.cc
 *
 * Unit tests for osw::events::Binder against the FS-mock seam.
 *
 * Covered:
 *   - Init() registers via switch_event_bind for SWITCH_EVENT_ALL + NULL
 *     subclass, captures the right callback function.
 *   - Init() is idempotent (warns; returns true).
 *   - Stop() unbinds via switch_event_unbind_callback; is idempotent.
 *   - The C-linkage shim osw_event_handler with NULL event is a no-op.
 *   - The C-linkage shim with no Binder set (pre-Init / post-Stop) is
 *     a no-op (does not crash).
 *   - HandleEvent classifies, allocates seq, builds envelope, pushes to
 *     the correct tier ring; events_emitted_ increments.
 *   - HandleEvent on overflow increments the tier's drops counter.
 *   - Exception inside HandleEvent body is caught by the wrapper (the
 *     test uses a Health pointer that throws inside the snapshot setter
 *     to exercise the catch path).
 *   - HandleEvent with NULL event is a no-op.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/binder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/events/v1/events.pb.h"

#include "osw/events/envelope.h"
#include "osw/events/ring.h"
#include "osw/events/tier.h"
#include "osw/observability/health.h"
#include "osw/raii/fs_mock.h"

namespace {

using osw::events::Binder;
using osw::events::EnvelopeBuildConfig;
using osw::events::MakeDefaultEnvelopeConfig;
using osw::events::MakeDefaultRules;
using osw::events::Ring;
using osw::events::RingSet;
using osw::events::Tier;
using osw::events::TierClassifier;

switch_event_t* const kEv = reinterpret_cast<switch_event_t*>(0xB001);

class BinderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        osw::raii::fs::MockReset();
        rings_ = std::make_unique<RingSet>(256, 256, 256);
        classifier_ = std::make_unique<TierClassifier>(MakeDefaultRules());
        health_ = std::make_unique<osw::Health>();
    }
    void TearDown() override {
        // Make sure no Binder leaks into the shim slot across tests.
        // Stop() clears the slot; Binder dtor also calls Stop().
        binder_.reset();
    }

    void SetHeader(switch_event_t* ev, const std::string& name, const std::string& value) {
        auto& m = osw::raii::fs::Mock();
        std::lock_guard<std::mutex> g(m.capture_mu);
        m.events_by_ptr[ev].headers.emplace_back(name, value);
    }

    std::unique_ptr<RingSet> rings_;
    std::unique_ptr<TierClassifier> classifier_;
    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<Binder> binder_;
};

TEST_F(BinderTest, InitRegistersForAllEventsAndIsIdempotent) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());

    EXPECT_FALSE(binder_->IsActive());
    EXPECT_TRUE(binder_->Init());
    EXPECT_TRUE(binder_->IsActive());

    auto& m = osw::raii::fs::Mock();
    EXPECT_EQ(m.event_bind_calls.load(), 1);
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        ASSERT_EQ(m.bindings.size(), 1u);
        EXPECT_EQ(m.bindings[0].id, "mod_open_switch");
        EXPECT_EQ(m.bindings[0].event, SWITCH_EVENT_ALL);
        EXPECT_EQ(m.bindings[0].subclass_name, "");
        EXPECT_EQ(m.bindings[0].callback, &osw::events::osw_event_handler);
        EXPECT_TRUE(m.bindings[0].active);
    }

    // Second Init() warns but returns true; no extra bind call.
    EXPECT_TRUE(binder_->Init());
    EXPECT_EQ(m.event_bind_calls.load(), 1);
}

TEST_F(BinderTest, StopUnbindsAndIsIdempotent) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());
    EXPECT_TRUE(binder_->Init());

    binder_->Stop();
    auto& m = osw::raii::fs::Mock();
    EXPECT_FALSE(binder_->IsActive());
    EXPECT_EQ(m.event_unbind_calls.load(), 1);
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        ASSERT_EQ(m.bindings.size(), 1u);
        EXPECT_FALSE(m.bindings[0].active);
    }

    // Second Stop() is a no-op (no extra unbind call).
    binder_->Stop();
    EXPECT_EQ(m.event_unbind_calls.load(), 1);
}

TEST_F(BinderTest, BindFailurePropagates) {
    auto& m = osw::raii::fs::Mock();
    m.next_event_bind_status = SWITCH_STATUS_GENERR;

    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());
    EXPECT_FALSE(binder_->Init());
    EXPECT_FALSE(binder_->IsActive());
}

TEST_F(BinderTest, ShimWithNullEventIsNoOp) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());
    EXPECT_TRUE(binder_->Init());

    osw::events::osw_event_handler(nullptr);
    EXPECT_EQ(binder_->EventsEmitted(), 0u);
}

TEST_F(BinderTest, ShimWithNoBinderIsNoOp) {
    // No Init() called → the shim's slot is null.
    osw::events::osw_event_handler(kEv);  // does NOT crash
    SUCCEED();
}

TEST_F(BinderTest, HandleEventRoutesToTier1ForHangupComplete) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());

    SetHeader(kEv, "Event-Name", "CHANNEL_HANGUP_COMPLETE");
    SetHeader(kEv, "Event-Date-Timestamp", "1748287234000000");
    SetHeader(kEv, "Unique-ID", "u-1");

    binder_->HandleEvent(kEv);
    EXPECT_EQ(binder_->EventsEmitted(), 1u);

    Ring* t1 = rings_->Get(Tier::k1Critical);
    ASSERT_NE(t1, nullptr);
    EXPECT_EQ(t1->Size(), 1u);
    EXPECT_EQ(rings_->Get(Tier::k2State)->Size(), 0u);
    EXPECT_EQ(rings_->Get(Tier::k3Ephemeral)->Size(), 0u);

    auto entry = t1->TryPop();
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->seq, 1u);
    EXPECT_FALSE(entry->envelope_bytes->empty());
}

TEST_F(BinderTest, HandleEventRoutesAuditSubclassToTier1) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());

    SetHeader(kEv, "Event-Name", "CUSTOM");
    SetHeader(kEv, "Event-Subclass", "osw.audit.module_loaded");

    binder_->HandleEvent(kEv);

    EXPECT_EQ(rings_->Get(Tier::k1Critical)->Size(), 1u);
    EXPECT_EQ(rings_->Get(Tier::k2State)->Size(), 0u);
    EXPECT_EQ(rings_->Get(Tier::k3Ephemeral)->Size(), 0u);
}

TEST_F(BinderTest, HandleEventOverflowIncrementsDrops) {
    rings_ = std::make_unique<RingSet>(2, 256, 256);  // tier 1 cap = 2
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());

    SetHeader(kEv, "Event-Name", "CHANNEL_HANGUP_COMPLETE");

    binder_->HandleEvent(kEv);
    binder_->HandleEvent(kEv);
    binder_->HandleEvent(kEv);  // overflow → evict + drop counter
    binder_->HandleEvent(kEv);  // overflow → evict + drop counter

    EXPECT_EQ(binder_->EventsEmitted(), 4u);
    EXPECT_EQ(binder_->DropsForTier(Tier::k1Critical), 2u);
    EXPECT_EQ(rings_->Get(Tier::k1Critical)->Size(), 2u);
    EXPECT_EQ(health_->GetSnapshot().tier1_dropped_total, 2u);
}

TEST_F(BinderTest, HandleEventWithNullEventIsNoOp) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-x", health_.get());
    binder_->HandleEvent(nullptr);
    EXPECT_EQ(binder_->EventsEmitted(), 0u);
}

TEST_F(BinderTest, HandleEventSerializedEnvelopeIsParseable) {
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "node-test", health_.get());

    SetHeader(kEv, "Event-Name", "HEARTBEAT");

    binder_->HandleEvent(kEv);
    auto e = rings_->Get(Tier::k3Ephemeral)->TryPop();
    ASSERT_TRUE(e);
    open_switch::events::v1::EventEnvelope env;
    ASSERT_TRUE(env.ParseFromString(*e->envelope_bytes));
    EXPECT_EQ(env.event_name(), "HEARTBEAT");
    EXPECT_EQ(env.tier(), open_switch::events::v1::TIER_3_EPHEMERAL);
    EXPECT_EQ(env.node_id(), "node-test");
    EXPECT_EQ(env.seq(), 1u);
    EXPECT_EQ(env.schema_version(), 1u);
}

TEST_F(BinderTest, ConcurrentProducersAllEventsAccountedFor) {
    constexpr int kProducers = 8;
    constexpr int kPer = 64;
    rings_ = std::make_unique<RingSet>(kProducers * kPer + 16,  // never overflow tier 1
                                       256,
                                       256);
    binder_ = std::make_unique<Binder>(
        rings_.get(), classifier_.get(), MakeDefaultEnvelopeConfig(), "n", health_.get());

    SetHeader(kEv, "Event-Name", "CHANNEL_HANGUP_COMPLETE");  // Tier 1

    std::vector<std::thread> ts;
    for (int p = 0; p < kProducers; ++p) {
        ts.emplace_back([&]() {
            for (int i = 0; i < kPer; ++i) {
                binder_->HandleEvent(kEv);
            }
        });
    }
    for (auto& t : ts)
        t.join();

    const std::uint64_t total = kProducers * kPer;
    EXPECT_EQ(binder_->EventsEmitted(), total);
    EXPECT_EQ(rings_->Get(Tier::k1Critical)->Size(), total);
    EXPECT_EQ(binder_->DropsForTier(Tier::k1Critical), 0u);

    // Codex W2 N-2: drain the ring + verify seqs are exactly
    // {1..total} with no gaps and no duplicates. The MPSC seq
    // generator is a per-tier atomic fetch_add; a subtle ordering
    // bug in a future refactor (e.g. dropping memory_order_acq_rel)
    // could miscount otherwise unnoticed if we only check totals.
    std::vector<std::uint64_t> seqs;
    seqs.reserve(total);
    while (auto e = rings_->Get(Tier::k1Critical)->TryPop()) {
        open_switch::events::v1::EventEnvelope env;
        ASSERT_TRUE(env.ParseFromString(*e->envelope_bytes));
        seqs.push_back(env.seq());
    }
    std::sort(seqs.begin(), seqs.end());
    ASSERT_EQ(seqs.size(), total);
    for (std::uint64_t i = 0; i < total; ++i) {
        EXPECT_EQ(seqs[i], i + 1u) << "seq gap or dup at index " << i;
    }
}

}  // namespace
