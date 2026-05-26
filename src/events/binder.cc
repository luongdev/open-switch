/*
 * src/events/binder.cc
 *
 * Implementation of osw::events::Binder.
 *
 * Build-mode contract (same as audit.cc + envelope.cc):
 *   - Production lib osw_events_fs: no OSW_TEST_FS_MOCK → fs_api.h
 *     trampolines onto real switch_event_bind / switch_event_unbind_callback.
 *   - Test lib osw_events_test_helpers: OSW_TEST_FS_MOCK=1 → fs_api.h
 *     includes fs_mock.h and the bind/unbind are captured into
 *     MockState::bindings.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/binder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <google/protobuf/arena.h>

#include "open_switch/events/v1/events.pb.h"

#include "osw/observability/health.h"
#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"

namespace osw::events {

// Forward-declare the shim-slot accessor so Init/Stop can publish into
// it before its definition at the bottom of the TU. Disambiguates from
// google::protobuf::internal (which the generated header also defines).
namespace internal {
std::atomic<Binder*>& BinderInstanceSlot() noexcept;
}  // namespace internal

namespace {

constexpr const char* kSubsystem = "events.binder";
constexpr const char* kBindId = "mod_open_switch";

// FF-018 bind: SWITCH_EVENT_ALL with NULL subclass = receive every
// event. The classifier later filters / maps to tiers in-process.
constexpr int kSwitchEventAll = SWITCH_EVENT_ALL;

std::size_t TierIndex(Tier t) noexcept {
    switch (t) {
        case Tier::k1Critical:
            return 0;
        case Tier::k2State:
            return 1;
        case Tier::k3Ephemeral:
            return 2;
        case Tier::kUnspecified:
        default:
            return 2;  // unspecified falls to Tier 3 slot
    }
}

}  // namespace

// --- RingSet ---------------------------------------------------------

RingSet::RingSet(std::size_t cap1, std::size_t cap2, std::size_t cap3)
    : tier1_(cap1), tier2_(cap2), tier3_(cap3) {}

Ring* RingSet::Get(Tier t) noexcept {
    switch (t) {
        case Tier::k1Critical:
            return &tier1_;
        case Tier::k2State:
            return &tier2_;
        case Tier::k3Ephemeral:
            return &tier3_;
        case Tier::kUnspecified:
        default:
            return nullptr;
    }
}

void RingSet::CloseAll() noexcept {
    tier1_.Close();
    tier2_.Close();
    tier3_.Close();
}

bool RingSet::AllEmpty() const noexcept {
    return tier1_.Size() == 0 && tier2_.Size() == 0 && tier3_.Size() == 0;
}

// --- Binder ----------------------------------------------------------

Binder::Binder(RingSet* rings,
               const TierClassifier* classifier,
               EnvelopeBuildConfig envelope_cfg,
               std::string node_id,
               Health* health) noexcept
    : rings_(rings),
      classifier_(classifier),
      envelope_cfg_(std::move(envelope_cfg)),
      node_id_(std::move(node_id)),
      health_(health) {
    for (auto& s : next_seq_)
        s.store(0, std::memory_order_relaxed);
    for (auto& d : dropped_)
        d.store(0, std::memory_order_relaxed);
}

Binder::~Binder() noexcept {
    // Defensive: if Stop() wasn't called, do it now. FF-018 unbind
    // under wrlock is safe to call multiple times in our shim
    // (returns FALSE the second time).
    Stop();
}

bool Binder::Init() noexcept {
    if (active_.load(std::memory_order_acquire)) {
        osw::log::Warn(kSubsystem, "Binder::Init called twice; ignoring");
        return true;
    }
    // Publish `this` to the C-linkage shim BEFORE the FS bind so that
    // any event arriving immediately after switch_event_bind returns
    // finds a valid Binder. Cleared on Stop().
    Binder* expected = nullptr;
    if (!internal::BinderInstanceSlot().compare_exchange_strong(
            expected, this, std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Another Binder is already active — disallow.
        osw::log::Error(kSubsystem,
                        "Binder::Init refused — another Binder instance is "
                        "already published to the shim");
        return false;
    }
    try {
        // FF-018 bind. SWITCH_EVENT_ALL + NULL subclass = receive every
        // event. The user_data slot is the Binder instance, kept for
        // future use; the shim uses the process-singleton slot.
        const switch_status_t s =
            ::osw::raii::fs::EventBind(kBindId,
                                       static_cast<switch_event_types_t>(kSwitchEventAll),
                                       /*subclass_name=*/nullptr,
                                       &osw_event_handler,
                                       this);
        if (s != SWITCH_STATUS_SUCCESS) {
            osw::log::Error(kSubsystem, "switch_event_bind failed: status=%d", static_cast<int>(s));
            internal::BinderInstanceSlot().store(nullptr, std::memory_order_release);
            return false;
        }
        active_.store(true, std::memory_order_release);
        osw::log::Info(kSubsystem,
                       "Binder::Init bound osw_event_handler for ALL events (node_id='%s')",
                       node_id_.c_str());
        return true;
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "Binder::Init threw: %s", e.what());
        internal::BinderInstanceSlot().store(nullptr, std::memory_order_release);
        return false;
    } catch (...) {
        osw::log::Error(kSubsystem, "Binder::Init threw unknown exception");
        internal::BinderInstanceSlot().store(nullptr, std::memory_order_release);
        return false;
    }
}

void Binder::Stop() noexcept {
    // Serialise Stop() across callers; one wins, the rest no-op.
    std::lock_guard<std::mutex> lk(stop_mu_);
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        // FF-018 unbind waits under wrlock for any in-flight dispatch
        // to release the rdlock — after this returns, no further
        // osw_event_handler invocations will happen.
        const switch_status_t s = ::osw::raii::fs::EventUnbindCallback(&osw_event_handler);
        if (s != SWITCH_STATUS_SUCCESS) {
            // FS-side returns FALSE when no binding existed — non-fatal.
            osw::log::Debug(kSubsystem,
                            "switch_event_unbind_callback returned %d "
                            "(likely already unbound)",
                            static_cast<int>(s));
        }
        active_.store(false, std::memory_order_release);
        // Clear the shim's instance slot ONLY after the FS unbind
        // returns: FF-018 guarantees in-flight dispatch threads have
        // completed by the time wrlock returns, so the shim's
        // `b->HandleEvent(event)` cannot race against destruction.
        internal::BinderInstanceSlot().store(nullptr, std::memory_order_release);
        osw::log::Info(kSubsystem, "Binder::Stop unbound osw_event_handler");
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "Binder::Stop threw: %s", e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "Binder::Stop threw unknown exception");
    }
}

void Binder::HandleEvent(switch_event_t* ev) noexcept {
    // Per the W2 contract §"Critical implementation discipline" rule
    // #6, the FULL body lives inside try/catch. Even though most of
    // what we call is noexcept-or-equivalent, std::string/proto
    // allocations can throw bad_alloc, and we MUST NOT let that escape
    // into FS's C dispatcher (FF-018).
    try {
        if (ev == nullptr || rings_ == nullptr || classifier_ == nullptr) {
            // Defensive — Init order in Module guarantees these are non-null.
            return;
        }

        // FF-019: header reads return FS-owned pointers, lifetime ≤ ev.
        // We synchronously read Event-Name + Event-Subclass for the
        // classifier, then BuildEnvelope copies everything else.
        const char* en_raw = ::osw::raii::fs::EventGetHeader(ev, "Event-Name");
        const char* sc_raw = ::osw::raii::fs::EventGetHeader(ev, "Event-Subclass");
        const std::string_view event_name = en_raw ? std::string_view(en_raw) : "";
        const std::string_view subclass_name = sc_raw ? std::string_view(sc_raw) : "";

        const Tier tier = classifier_->Classify(event_name, subclass_name);
        Ring* ring = rings_->Get(tier);
        if (ring == nullptr) {
            osw::log::Warn(kSubsystem,
                           "no ring for tier=%d (event=%s); dropping",
                           static_cast<int>(tier),
                           std::string(event_name).c_str());
            return;
        }
        const std::size_t tidx = TierIndex(tier);

        // Per-tier sequence allocation. fetch_add returns the
        // pre-increment value; +1 makes seqs start at 1.
        const std::uint64_t seq = next_seq_[tidx].fetch_add(1, std::memory_order_relaxed) + 1;

        // BuildEnvelope returns an arena-owned proto. We serialize to
        // a fresh std::string and ship via shared_ptr<const string>.
        // The arena is short-lived (RAII-on-stack) and destroyed when
        // the function exits.
        google::protobuf::Arena arena;
        open_switch::events::v1::EventEnvelope* env =
            BuildEnvelope(ev, tier, seq, node_id_, envelope_cfg_, &arena);
        if (env == nullptr) {
            // BuildEnvelope already logged.
            return;
        }

        auto bytes = std::make_shared<std::string>();
        if (!env->SerializeToString(bytes.get())) {
            osw::log::Warn(kSubsystem,
                           "SerializeToString failed for event=%s seq=%llu",
                           std::string(event_name).c_str(),
                           static_cast<unsigned long long>(seq));
            return;
        }
        std::shared_ptr<const std::string> bytes_const = std::move(bytes);

        RingEntry entry;
        entry.seq = seq;
        entry.envelope_bytes = std::move(bytes_const);

        std::uint64_t local_dropped = 0;
        ring->Push(std::move(entry), &local_dropped);
        if (local_dropped > 0) {
            dropped_[tidx].fetch_add(local_dropped, std::memory_order_relaxed);
        }
        events_emitted_.fetch_add(1, std::memory_order_relaxed);

        IncrementHealthCounters();
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "HandleEvent threw: %s", e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "HandleEvent threw unknown exception");
    }
}

std::uint64_t Binder::DropsForTier(Tier t) const noexcept {
    return dropped_[TierIndex(t)].load(std::memory_order_relaxed);
}

void Binder::IncrementHealthCounters() noexcept {
    if (health_ == nullptr)
        return;
    // events_emitted is the raw counter; SetEventsEmittedTotal accepts
    // the latest value (atomic store inside Health). Per-tier dropped
    // counters and ring fills are surfaced separately so the broadcaster
    // can refresh them on its own cadence (the Binder updates them at
    // event time for promptness).
    health_->SetEventsEmittedTotal(events_emitted_.load(std::memory_order_relaxed));

    if (rings_ != nullptr) {
        auto set_fill = [&](Tier t, void (Health::*setter)(std::uint32_t)) {
            if (Ring* r = rings_->Get(t)) {
                const std::size_t sz = r->Size();
                const std::size_t cap = r->Capacity();
                const std::uint32_t pct =
                    cap == 0 ? 0 : static_cast<std::uint32_t>((sz * 100) / cap);
                (health_->*setter)(pct);
            }
        };
        set_fill(Tier::k1Critical, &Health::SetTier1RingFillPct);
        set_fill(Tier::k2State, &Health::SetTier2RingFillPct);
        set_fill(Tier::k3Ephemeral, &Health::SetTier3RingFillPct);

        health_->SetTier1DroppedTotal(dropped_[0].load(std::memory_order_relaxed));
        health_->SetTier2DroppedTotal(dropped_[1].load(std::memory_order_relaxed));
        health_->SetTier3DroppedTotal(dropped_[2].load(std::memory_order_relaxed));
    }
}

// --- C-linkage shim --------------------------------------------------
//
// FF-018 callback typedef is `void(*)(switch_event_t*)` — no user_data
// argument in the callback signature itself. FS does set
// `(*event)->bind_user_data` to the void* that switch_event_bind
// received, but our event-handling code prefers to locate the Binder
// via a process-singleton atomic so the body stays out of the
// switch_event_t internals (and so the bind_user_data slot is free
// for any future per-bind cookie). Init() publishes; Stop() clears.
//
// One Binder is registered at a time — Init() rejects a second call
// while active. Stop() blocks until in-flight HandleEvent calls
// complete (FS's wrlock waits on the rdlock; see FF-018).

namespace internal {
std::atomic<Binder*>& BinderInstanceSlot() noexcept {
    static std::atomic<Binder*> slot{nullptr};
    return slot;
}
}  // namespace internal

extern "C" void osw_event_handler(switch_event_t* event) {
    // FULL try/catch boundary: nothing escapes into the FS C dispatcher.
    try {
        if (event == nullptr) {
            return;
        }
        Binder* b = internal::BinderInstanceSlot().load(std::memory_order_acquire);
        if (b == nullptr) {
            // Either before Init() or after Stop(). Safe no-op; FF-018's
            // unbind-wrlock-wait protects us from the post-Stop race
            // (in-flight dispatch threads have already returned by the
            // time Stop() clears the slot).
            return;
        }
        b->HandleEvent(event);
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "osw_event_handler shim threw: %s", e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "osw_event_handler shim threw unknown exception");
    }
}

}  // namespace osw::events
