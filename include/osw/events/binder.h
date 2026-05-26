/*
 * include/osw/events/binder.h
 *
 * osw::events::Binder — owns the switch_event_bind registration that
 * drives the W2 event plane.
 *
 * Lifecycle (per W2 contract §"Module wiring updates"):
 *
 *   Module::Load (after grpc_server_->Start()):
 *     auto rings = std::make_unique<RingSet>(...);
 *     auto bcast = std::make_unique<Broadcaster>(...);
 *     binder_     = std::make_unique<Binder>(rings.get(), classifier_,
 *                                             envelope_cfg_, node_id_,
 *                                             &health_);
 *     binder_->Init();   // calls switch_event_bind
 *     audit::Emit("module_loaded", {...});
 *
 *   Module::Shutdown:
 *     lifecycle_.SignalDrain();           // Health → DRAINING
 *     binder_->Stop();                    // FF-018 wrlock → no new events
 *     rings.Close + WaitForDrain(deadline);
 *     bcast->Stop();
 *     audit::Emit(...) if pending
 *     grpc_server_->Drain(...);
 *
 * Threading:
 *   - HandleEvent is invoked concurrently from any of FS's up-to-64
 *     dispatch threads (FF-004). The body MUST be reentrant; per-tier
 *     producer state uses std::atomic<uint64_t> for sequence allocation
 *     and the Ring's MPSC-safe Push() for enqueue.
 *   - Init() / Stop() are called exactly once each, from the Module
 *     singleton's serialised Load / Shutdown paths. They are NOT
 *     thread-safe against concurrent self-invocation but ARE safe
 *     against concurrent HandleEvent (FF-018 unbind under wrlock
 *     waits for in-flight dispatch).
 *
 * Exception boundary:
 *   - osw_event_handler() (C-linkage shim) wraps HandleEvent() in a
 *     try { ... } catch (std::exception&) { ... } catch (...) { ... }
 *     per W2 contract §"Critical implementation discipline" rule #6.
 *     Exceptions are logged and SWALLOWED. They MUST NOT propagate
 *     into FS's C dispatcher (UB; FF-018 implication).
 *
 * Memory:
 *   - The Binder holds a non-owning view of the RingSet. The Module
 *     singleton owns the RingSet; teardown order is Stop() (no new
 *     producers) → ring drain → broadcaster shutdown → RingSet
 *     destruction.
 *   - HandleEvent serialises the envelope to bytes inside the
 *     callback (FF-018: pointer is FS-owned, callback-scope), then
 *     pushes a shared_ptr<const string> into the tier ring. No FS
 *     pointer ever escapes the callback.
 *
 * Optional debug timing:
 *   - OSW_DEBUG_TIMING=1 build flag wires a synthetic histogram around
 *     HandleEvent — disabled in release builds. The W2 target is
 *     ≤ 50µs steady-state per call; > 200µs is the "stop and surface"
 *     threshold (W2 contract).
 *
 * FACTs cited:
 *   - FF-004: 64 dispatch threads → MPSC ring.
 *   - FF-018: callback ownership, unbind under wrlock.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_BINDER_H_
#define OSW_EVENTS_BINDER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "osw/events/envelope.h"
#include "osw/events/ring.h"
#include "osw/events/tier.h"

// Forward-declare to keep this header thin.
extern "C" struct switch_event;
using switch_event_t = switch_event;

namespace osw {
class Health;

namespace events {

/// A bundle of three Rings, one per tier. Owned by the Module
/// singleton; Binder + Broadcaster hold non-owning views.
class RingSet {
  public:
    RingSet(std::size_t capacity_tier1, std::size_t capacity_tier2, std::size_t capacity_tier3);

    RingSet(const RingSet&) = delete;
    RingSet& operator=(const RingSet&) = delete;

    /// Access the ring for a tier. Returns nullptr for kUnspecified.
    [[nodiscard]] Ring* Get(Tier t) noexcept;

    /// Close all three rings; the broadcaster's WaitAndPopBatch wakes up.
    void CloseAll() noexcept;

    /// Returns true when all three rings have Size() == 0. Used by the
    /// Module's drain-wait loop.
    [[nodiscard]] bool AllEmpty() const noexcept;

    /// Block until either AllEmpty() is true or `deadline` is reached.
    /// Returns true on full drain, false on timeout. Gemini W2.5 I-4:
    /// replaces the 10ms `std::this_thread::sleep_for` busy-poll that
    /// was in Module::Shutdown. The wait is driven by a condvar that
    /// each ring's drain-notifier signals when it transitions to empty.
    ///
    /// Thread-safety:
    ///   - Multiple callers may wait simultaneously (all are notified
    ///     when AllEmpty becomes true).
    ///   - May be called from any thread; the notification side runs
    ///     inside the broadcaster's per-tier worker WaitAndPopBatch
    ///     callsite, so the wakeup is observed promptly.
    [[nodiscard]] bool WaitUntilAllEmpty(std::chrono::steady_clock::time_point deadline) noexcept;

  private:
    Ring tier1_;
    Ring tier2_;
    Ring tier3_;

    // I-4 drain notification. Lock-order safety:
    //
    //   - The drain-notifier callbacks each Ring runs are invoked WITH
    //     that ring's mu_ held. The callback acquires `drain_mu_` and
    //     bumps `drain_generation_`. New lock order: ring_mu → drain_mu.
    //
    //   - The waiter (`WaitUntilAllEmpty`) holds drain_mu_ inside
    //     condition_variable::wait_until's predicate. It must NEVER
    //     call back into any Ring under drain_mu_ — that would reverse
    //     the order (drain_mu → ring_mu) and deadlock with the
    //     notifier path.
    //
    // The predicate therefore checks only a snapshot of
    // `drain_generation_` (an integer comparison). `AllEmpty()` is
    // called OUT of the drain_mu_ critical section, in the loop body
    // that wraps the wait. The generation counter guarantees we never
    // miss a drain-empty transition: if the rings are empty *after*
    // the loop's last AllEmpty() check, either generation incremented
    // (we'll re-enter wait_until → wake immediately) or we never
    // entered wait_until (we'll return true on the next iteration).
    mutable std::mutex drain_mu_;
    std::condition_variable drain_cv_;
    std::uint64_t drain_generation_ = 0;  // guarded by drain_mu_

    // Helper that all three Rings' drain-notifier callbacks share.
    // Bumps drain_generation_ + notify_all so every shutdown waiter
    // wakes and re-checks AllEmpty() out-of-lock.
    void NotifyDrainTransition() noexcept;
};

class Binder {
  public:
    /// `rings`, `classifier`, `health` are non-owning. The Module
    /// singleton owns them; the Binder borrows.
    /// `node_id` is captured by value; used as the EventEnvelope.node_id
    /// for every emitted envelope. `envelope_cfg` is the operator's
    /// include-list configuration (default at MakeDefaultEnvelopeConfig()).
    Binder(RingSet* rings,
           const TierClassifier* classifier,
           EnvelopeBuildConfig envelope_cfg,
           std::string node_id,
           Health* health) noexcept;

    ~Binder() noexcept;

    Binder(const Binder&) = delete;
    Binder& operator=(const Binder&) = delete;
    Binder(Binder&&) = delete;
    Binder& operator=(Binder&&) = delete;

    /// Register `osw_event_handler` with the FS event facility via
    /// switch_event_bind (FF-018). Returns true on success. Safe to
    /// call exactly once. Subsequent calls log a warning and return
    /// true (idempotent).
    bool Init() noexcept;

    /// Unregister via switch_event_unbind_callback. FF-018 guarantees
    /// no further dispatch after this returns. Safe to call from any
    /// thread; safe to call multiple times.
    void Stop() noexcept;

    /// True iff Init() succeeded and Stop() has not yet been called.
    [[nodiscard]] bool IsActive() const noexcept { return active_.load(std::memory_order_acquire); }

    // --- Internals exposed for testing ---------------------------------

    /// The actual handler body. Called by the C-linkage shim
    /// `osw_event_handler` (which provides the try/catch boundary) AND
    /// directly by unit tests (which exercise the body without going
    /// through switch_event_bind).
    void HandleEvent(switch_event_t* ev) noexcept;

    /// Per-tier event counter readouts. Used by tests + Health.
    [[nodiscard]] std::uint64_t EventsEmitted() const noexcept {
        return events_emitted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t DropsForTier(Tier t) const noexcept;

  private:
    void IncrementHealthCounters() noexcept;

    RingSet* rings_;
    const TierClassifier* classifier_;
    EnvelopeBuildConfig envelope_cfg_;
    std::string node_id_;
    Health* health_;

    // Per-tier monotonic sequence counters. fetch_add returns the
    // pre-increment value; the producer uses (returned + 1) as the
    // seq so seq numbers start at 1.
    std::array<std::atomic<std::uint64_t>, 3> next_seq_;  // tier1/2/3
    std::array<std::atomic<std::uint64_t>, 3> dropped_;   // counts evictions

    std::atomic<std::uint64_t> events_emitted_{0};
    std::atomic<bool> active_{false};
    std::mutex stop_mu_;  // serialises Stop()
};

/// The C-linkage shim that switch_event_bind invokes. Defined in
/// binder.cc. The address of this function is registered via
/// switch_event_bind and matched by switch_event_unbind_callback. The
/// `bind_user_data` slot points back at the Binder instance.
extern "C" void osw_event_handler(switch_event_t* event);

}  // namespace events
}  // namespace osw

#endif  // OSW_EVENTS_BINDER_H_
