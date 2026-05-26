/*
 * tests/unit/events/broadcaster_test.cc
 *
 * Unit tests for osw::events::Broadcaster + Subscriber + SendQueue.
 *
 * Covered:
 *   - ProcessOneForTesting routes by tier filter.
 *   - ProcessOneForTesting routes by event_name glob.
 *   - ProcessOneForTesting routes by node_id filter.
 *   - SendQueue full → subscriber kicked with kQueueFull; kick counter
 *     increments; broadcaster does NOT block.
 *   - Slow subscriber does NOT back-pressure a fast subscriber: when
 *     one subscriber's queue is full and gets kicked, the other
 *     receives all events.
 *   - Clean Start/Stop shuts down 3 worker threads; all subscribers
 *     get kShutdown.
 *   - End-to-end via Start(): producer fills ring, broadcaster
 *     drains, subscriber's send queue receives the entry.
 *   - Filter: closed subscriber receives no further events.
 *
 * The broadcaster operates on serialised envelope bytes; we build a
 * minimal valid proto here to exercise the routing-fields scanner.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/subscribe/broadcaster.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "open_switch/events/v1/events.pb.h"

#include "osw/events/binder.h"  // RingSet full definition
#include "osw/events/ring.h"
#include "osw/events/subscribe/send_queue.h"
#include "osw/events/subscribe/subscriber.h"
#include "osw/events/tier.h"
#include "osw/observability/health.h"

namespace {

using osw::events::Broadcaster;
using osw::events::KickReason;
using osw::events::Ring;
using osw::events::RingEntry;
using osw::events::RingSet;
using osw::events::SendQueue;
using osw::events::Subscriber;
using osw::events::SubscriberFilter;
using osw::events::Tier;

// Build a serialised EventEnvelope with the routing fields populated.
// We don't depend on the full envelope builder (which calls into FS) —
// the broadcaster needs only tier/event_name/node_id (and optionally
// subclass_name) in the wire bytes.
std::shared_ptr<const std::string> SerializeEnvelope(Tier tier,
                                                     const std::string& event_name,
                                                     const std::string& node_id,
                                                     std::uint64_t seq = 1,
                                                     const std::string& subclass_name = "") {
    open_switch::events::v1::EventEnvelope env;
    env.set_event_id("ev-" + std::to_string(seq));
    switch (tier) {
        case Tier::k1Critical:
            env.set_tier(open_switch::events::v1::TIER_1_CRITICAL);
            break;
        case Tier::k2State:
            env.set_tier(open_switch::events::v1::TIER_2_STATE);
            break;
        case Tier::k3Ephemeral:
            env.set_tier(open_switch::events::v1::TIER_3_EPHEMERAL);
            break;
        default:
            env.set_tier(open_switch::events::v1::TIER_UNSPECIFIED);
            break;
    }
    env.set_event_name(event_name);
    if (!subclass_name.empty()) {
        env.set_subclass_name(subclass_name);
    }
    env.set_node_id(node_id);
    env.set_seq(seq);
    env.set_schema_version(1);
    auto bytes = std::make_shared<std::string>();
    EXPECT_TRUE(env.SerializeToString(bytes.get()));
    return bytes;
}

RingEntry MakeEntry(std::uint64_t seq,
                    Tier tier,
                    const std::string& name,
                    const std::string& node_id = "n1") {
    return RingEntry{seq, SerializeEnvelope(tier, name, node_id, seq)};
}

RingEntry MakeCustomEntry(std::uint64_t seq,
                          Tier tier,
                          const std::string& subclass_name,
                          const std::string& node_id = "n1") {
    return RingEntry{seq, SerializeEnvelope(tier, "CUSTOM", node_id, seq, subclass_name)};
}

class BroadcasterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        rings_ = std::make_unique<RingSet>(64, 64, 64);
        health_ = std::make_unique<osw::Health>();
        bcast_ = std::make_unique<Broadcaster>(rings_.get(), health_.get());
    }

    std::shared_ptr<Subscriber> MakeSub(const std::string& id,
                                        SubscriberFilter filter,
                                        std::size_t cap = 16) {
        return std::make_shared<Subscriber>(id, std::move(filter), cap);
    }

    std::unique_ptr<RingSet> rings_;
    std::unique_ptr<osw::Health> health_;
    std::unique_ptr<Broadcaster> bcast_;
};

TEST_F(BroadcasterTest, RouteByTierFilter) {
    SubscriberFilter f1;
    f1.tiers.insert(Tier::k1Critical);
    auto sub_t1 = MakeSub("s-t1", f1);

    SubscriberFilter f3;
    f3.tiers.insert(Tier::k3Ephemeral);
    auto sub_t3 = MakeSub("s-t3", f3);

    bcast_->AddSubscriber(sub_t1);
    bcast_->AddSubscriber(sub_t3);

    bcast_->ProcessOneForTesting(Tier::k1Critical,
                                 MakeEntry(1, Tier::k1Critical, "CHANNEL_HANGUP_COMPLETE"));

    EXPECT_EQ(sub_t1->Queue().Size(), 1u);
    EXPECT_EQ(sub_t3->Queue().Size(), 0u);
}

TEST_F(BroadcasterTest, RouteByEventNameGlob) {
    SubscriberFilter f;
    f.event_name_globs = {"CHANNEL_*"};
    auto sub = MakeSub("s-ch", f);
    bcast_->AddSubscriber(sub);

    bcast_->ProcessOneForTesting(Tier::k1Critical,
                                 MakeEntry(1, Tier::k1Critical, "CHANNEL_HANGUP_COMPLETE"));
    bcast_->ProcessOneForTesting(Tier::k3Ephemeral, MakeEntry(2, Tier::k3Ephemeral, "HEARTBEAT"));

    EXPECT_EQ(sub->Queue().Size(), 1u);
}

TEST_F(BroadcasterTest, RouteByExactEventName) {
    SubscriberFilter f;
    f.event_name_globs = {"DTMF"};  // exact, no '*'
    auto sub = MakeSub("s-dtmf", f);
    bcast_->AddSubscriber(sub);

    bcast_->ProcessOneForTesting(Tier::k2State, MakeEntry(1, Tier::k2State, "DTMF"));
    bcast_->ProcessOneForTesting(Tier::k2State, MakeEntry(2, Tier::k2State, "DTMF_OTHER"));

    EXPECT_EQ(sub->Queue().Size(), 1u);
}

TEST_F(BroadcasterTest, RouteByNodeIdFilter) {
    SubscriberFilter f;
    f.node_id = "node-a";
    auto sub = MakeSub("s", f);
    bcast_->AddSubscriber(sub);

    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(1, Tier::k1Critical, "EV", "node-a"));
    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(2, Tier::k1Critical, "EV", "node-b"));

    EXPECT_EQ(sub->Queue().Size(), 1u);
}

// Gemini W2.5 C-2: end-to-end live-tail check that the routing.cc
// scanner correctly extracts subclass_name (proto tag 4) and that
// Subscriber::MatchesFilter rejects/accepts CUSTOM events by subclass.
TEST_F(BroadcasterTest, RouteByCustomSubclassGlob) {
    SubscriberFilter f;
    f.subclass_globs = {"osw.audit.*"};
    auto sub = MakeSub("s-audit", f);
    bcast_->AddSubscriber(sub);

    // Matching subclass — should be dispatched.
    bcast_->ProcessOneForTesting(Tier::k1Critical,
                                 MakeCustomEntry(1, Tier::k1Critical, "osw.audit.module_loaded"));
    // Non-matching subclass — should NOT be dispatched.
    bcast_->ProcessOneForTesting(Tier::k1Critical,
                                 MakeCustomEntry(2, Tier::k1Critical, "other.subclass.thing"));
    // Non-CUSTOM event (empty subclass) — should NOT be dispatched
    // because the subscriber has an active subclass filter.
    bcast_->ProcessOneForTesting(Tier::k1Critical,
                                 MakeEntry(3, Tier::k1Critical, "CHANNEL_ANSWER"));

    EXPECT_EQ(sub->Queue().Size(), 1u);
}

TEST_F(BroadcasterTest, EmptyFilterMatchesAll) {
    auto sub = MakeSub("s-all", SubscriberFilter{});
    bcast_->AddSubscriber(sub);

    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(1, Tier::k1Critical, "A"));
    bcast_->ProcessOneForTesting(Tier::k2State, MakeEntry(2, Tier::k2State, "B"));
    bcast_->ProcessOneForTesting(Tier::k3Ephemeral, MakeEntry(3, Tier::k3Ephemeral, "C"));

    EXPECT_EQ(sub->Queue().Size(), 3u);
}

TEST_F(BroadcasterTest, QueueFullKicksSubscriber) {
    auto sub = MakeSub("s-slow", SubscriberFilter{}, /*cap=*/2);
    bcast_->AddSubscriber(sub);

    // Fill the queue.
    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(1, Tier::k1Critical, "E"));
    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(2, Tier::k1Critical, "E"));
    EXPECT_EQ(sub->Queue().Size(), 2u);
    EXPECT_FALSE(sub->IsClosed());

    // 3rd entry overflows → subscriber kicked.
    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(3, Tier::k1Critical, "E"));
    EXPECT_TRUE(sub->IsClosed());
    EXPECT_EQ(sub->GetKickReason(), KickReason::kQueueFull);
    EXPECT_EQ(bcast_->KicksForReason(KickReason::kQueueFull), 1u);
}

TEST_F(BroadcasterTest, SlowSubscriberDoesNotBackpressureFastOne) {
    // Slow has cap=1; Fast has cap=64.
    auto slow = MakeSub("s-slow", SubscriberFilter{}, /*cap=*/1);
    auto fast = MakeSub("s-fast", SubscriberFilter{}, /*cap=*/64);
    bcast_->AddSubscriber(slow);
    bcast_->AddSubscriber(fast);

    for (std::uint64_t i = 1; i <= 8; ++i) {
        bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(i, Tier::k1Critical, "E"));
    }

    // Slow: kicked after 2nd dispatch (cap=1; first one fills, second
    // gets RESOURCE_EXHAUSTED). Note: once closed, subsequent
    // dispatches are skipped (IsClosed gate).
    EXPECT_TRUE(slow->IsClosed());
    EXPECT_EQ(slow->GetKickReason(), KickReason::kQueueFull);

    // Fast: receives all 8.
    EXPECT_EQ(fast->Queue().Size(), 8u);
    EXPECT_FALSE(fast->IsClosed());
    EXPECT_GE(bcast_->KicksForReason(KickReason::kQueueFull), 1u);
}

TEST_F(BroadcasterTest, ClosedSubscriberSkippedOnFurtherDispatch) {
    auto sub = MakeSub("s", SubscriberFilter{}, /*cap=*/16);
    bcast_->AddSubscriber(sub);

    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(1, Tier::k1Critical, "E"));
    EXPECT_EQ(sub->Queue().Size(), 1u);

    sub->RequestClose(KickReason::kClientCancelled);

    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(2, Tier::k1Critical, "E"));
    EXPECT_EQ(sub->Queue().Size(), 1u);  // unchanged — closed before push
}

TEST_F(BroadcasterTest, AddRemoveSubscriberAffectsRoster) {
    auto a = MakeSub("a", SubscriberFilter{});
    auto b = MakeSub("b", SubscriberFilter{});
    bcast_->AddSubscriber(a);
    bcast_->AddSubscriber(b);
    EXPECT_EQ(bcast_->SubscriberCount(), 2u);

    bcast_->RemoveSubscriber("a");
    EXPECT_EQ(bcast_->SubscriberCount(), 1u);

    bcast_->ProcessOneForTesting(Tier::k1Critical, MakeEntry(1, Tier::k1Critical, "E"));
    EXPECT_EQ(a->Queue().Size(), 0u);  // removed before dispatch
    EXPECT_EQ(b->Queue().Size(), 1u);
}

TEST_F(BroadcasterTest, StartStopJoinsAllThreads) {
    bcast_->Start();
    // No subscribers: workers idle on WaitAndPopBatch timeout.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bcast_->Stop();
    // Stop is idempotent.
    bcast_->Stop();
    SUCCEED();
}

TEST_F(BroadcasterTest, StopClosesAllSubscribersWithShutdownReason) {
    auto a = MakeSub("a", SubscriberFilter{});
    auto b = MakeSub("b", SubscriberFilter{});
    bcast_->AddSubscriber(a);
    bcast_->AddSubscriber(b);

    bcast_->Start();
    bcast_->Stop();

    EXPECT_TRUE(a->IsClosed());
    EXPECT_TRUE(b->IsClosed());
    EXPECT_EQ(a->GetKickReason(), KickReason::kShutdown);
    EXPECT_EQ(b->GetKickReason(), KickReason::kShutdown);
}

TEST_F(BroadcasterTest, EndToEndRingProducerDeliversToSubscriber) {
    auto sub = MakeSub("e2e", SubscriberFilter{}, /*cap=*/32);
    bcast_->AddSubscriber(sub);

    bcast_->Start();

    // Producer puts entries into the Tier-1 ring.
    Ring* t1 = rings_->Get(Tier::k1Critical);
    ASSERT_NE(t1, nullptr);

    for (std::uint64_t i = 1; i <= 5; ++i) {
        std::uint64_t dropped = 0;
        t1->Push(MakeEntry(i, Tier::k1Critical, "CHANNEL_HANGUP_COMPLETE"), &dropped);
    }

    // Wait up to 1s for delivery.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sub->Queue().Size() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(sub->Queue().Size(), 5u);

    bcast_->Stop();
}

TEST_F(BroadcasterTest, HealthSubscriberCountReflectsRoster) {
    EXPECT_EQ(health_->GetSnapshot().subscriber_count, 0u);
    auto a = MakeSub("a", SubscriberFilter{});
    bcast_->AddSubscriber(a);
    EXPECT_EQ(health_->GetSnapshot().subscriber_count, 1u);
    auto b = MakeSub("b", SubscriberFilter{});
    bcast_->AddSubscriber(b);
    EXPECT_EQ(health_->GetSnapshot().subscriber_count, 2u);
    bcast_->RemoveSubscriber("a");
    EXPECT_EQ(health_->GetSnapshot().subscriber_count, 1u);
}

// SendQueue tests --------------------------------------------------------

TEST(SendQueueTest, TryPushUntilFullThenRejects) {
    SendQueue q(2);
    EXPECT_TRUE(q.TryPush(std::make_shared<const std::string>("a")));
    EXPECT_TRUE(q.TryPush(std::make_shared<const std::string>("b")));
    EXPECT_FALSE(q.TryPush(std::make_shared<const std::string>("c")));
    EXPECT_EQ(q.Size(), 2u);
}

TEST(SendQueueTest, WaitAndPopFifo) {
    SendQueue q(8);
    EXPECT_TRUE(q.TryPush(std::make_shared<const std::string>("a")));
    EXPECT_TRUE(q.TryPush(std::make_shared<const std::string>("b")));
    auto a = q.WaitAndPop(std::chrono::milliseconds(10));
    auto b = q.WaitAndPop(std::chrono::milliseconds(10));
    ASSERT_TRUE(a && b);
    EXPECT_EQ(**a, "a");
    EXPECT_EQ(**b, "b");
}

TEST(SendQueueTest, WaitAndPopTimeoutEmpty) {
    SendQueue q(4);
    auto t0 = std::chrono::steady_clock::now();
    auto v = q.WaitAndPop(std::chrono::milliseconds(20));
    auto delta = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(v.has_value());
    EXPECT_GE(delta, std::chrono::milliseconds(15));
}

TEST(SendQueueTest, CloseUnblocksWaitAndPopAndRejectsFurtherPush) {
    SendQueue q(4);
    std::atomic<bool> awake{false};
    std::thread t([&]() {
        auto v = q.WaitAndPop(std::chrono::seconds(5));
        EXPECT_FALSE(v.has_value());
        awake.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.Close();
    t.join();
    EXPECT_TRUE(awake.load());
    EXPECT_TRUE(q.IsClosed());
    EXPECT_FALSE(q.TryPush(std::make_shared<const std::string>("x")));
}

// Subscriber tests -------------------------------------------------------

TEST(SubscriberTest, KickReasonFirstWriterWins) {
    Subscriber s("id-1", SubscriberFilter{}, 4);
    EXPECT_EQ(s.GetKickReason(), KickReason::kNone);

    s.RequestClose(KickReason::kQueueFull);
    EXPECT_EQ(s.GetKickReason(), KickReason::kQueueFull);

    // Subsequent close keeps the first reason.
    s.RequestClose(KickReason::kShutdown);
    EXPECT_EQ(s.GetKickReason(), KickReason::kQueueFull);
}

TEST(SubscriberTest, MatchesFilterEmptyAcceptsAll) {
    Subscriber s("id-1", SubscriberFilter{}, 4);
    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "ANY", "", "n"));
    EXPECT_TRUE(s.MatchesFilter(Tier::k3Ephemeral, "OTHER", "osw.audit.x", "x"));
}

TEST(SubscriberTest, MatchesFilterRejectsOnTierMismatch) {
    SubscriberFilter f;
    f.tiers.insert(Tier::k1Critical);
    Subscriber s("id-1", f, 4);
    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "X", "", "n"));
    EXPECT_FALSE(s.MatchesFilter(Tier::k3Ephemeral, "X", "", "n"));
}

// Gemini W2.5 C-2: subclass_globs filter is the prefix-wildcard glob
// matched against `Event-Subclass`. Empty list = match all subclasses.
TEST(SubscriberTest, MatchesFilterSubclassPrefixGlob) {
    SubscriberFilter f;
    f.subclass_globs = {"osw.audit.*"};
    Subscriber s("id-audit", f, 4);

    // Matching subclasses (prefix glob accepts).
    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "osw.audit.module_loaded", "n"));
    EXPECT_TRUE(
        s.MatchesFilter(Tier::k3Ephemeral, "CUSTOM", "osw.audit.subscriber_connected", "n"));

    // Non-matching subclass.
    EXPECT_FALSE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "other.subclass", "n"));

    // Non-CUSTOM event (empty subclass) is excluded when a subclass
    // filter is active — the empty string does NOT match `osw.audit.*`
    // because the prefix `osw.audit.` requires a 10-char match.
    EXPECT_FALSE(s.MatchesFilter(Tier::k1Critical, "CHANNEL_ANSWER", "", "n"));
}

TEST(SubscriberTest, MatchesFilterSubclassExactMatch) {
    SubscriberFilter f;
    f.subclass_globs = {"osw.audit.module_loaded"};
    Subscriber s("id-exact", f, 4);

    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "osw.audit.module_loaded", "n"));
    EXPECT_FALSE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "osw.audit.module_loaded2", "n"));
    EXPECT_FALSE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "", "n"));
}

TEST(SubscriberTest, MatchesFilterSubclassEmptyMatchesAll) {
    // No subclass_globs → subclass axis is unfiltered.
    SubscriberFilter f;
    Subscriber s("id-any", f, 4);
    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "anything", "n"));
    EXPECT_TRUE(s.MatchesFilter(Tier::k1Critical, "CUSTOM", "", "n"));
}

TEST(SubscriberTest, KickReasonToStringStable) {
    using osw::events::ToString;
    EXPECT_EQ(ToString(KickReason::kQueueFull), "queue_full");
    EXPECT_EQ(ToString(KickReason::kReplayEvicted), "replay_evicted");
    EXPECT_EQ(ToString(KickReason::kShutdown), "shutdown");
    EXPECT_EQ(ToString(KickReason::kClientCancelled), "client_cancelled");
    EXPECT_EQ(ToString(KickReason::kNone), "none");
}

}  // namespace
