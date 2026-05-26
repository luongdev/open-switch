/*
 * include/osw/core/lifecycle.h
 *
 * osw::Lifecycle — drain orchestration scaffolding.
 *
 * W1 implements only the bare bones:
 *   - State enum (Loaded / Serving / Draining / Stopped).
 *   - SignalDrain() flips state to Draining and updates osw::Health.
 *   - State() returns the current value atomically.
 *
 * The full drain sequence (event-ring flush, channel hupall, gRPC
 * server Shutdown deadline, etc.) lands as the owning subsystems
 * come online in W2/W3/W4. The state flag is enough for W1 — it
 * gates the Health RPC response and (in W3) the rejection of new
 * Originate / SubscribeEvents.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CORE_LIFECYCLE_H_
#define OSW_CORE_LIFECYCLE_H_

#include <atomic>

namespace osw {

class Health;  // forward decl; lifecycle.cc includes the header

class Lifecycle {
  public:
    enum class State : int {
        kLoaded = 0,    // construction complete, gRPC not yet started
        kServing = 1,   // gRPC bound and accepting RPCs
        kDraining = 2,  // SIGTERM observed; rejecting new RPCs
        kStopped = 3,   // shutdown returned
    };

    /// `health` is the module-wide Health aggregator. The Lifecycle
    /// keeps a non-owning view; ownership stays with the Module
    /// singleton.
    explicit Lifecycle(Health* health) noexcept : health_(health) {}

    [[nodiscard]] State GetState() const noexcept { return state_.load(std::memory_order_acquire); }

    /// Move to kServing. Idempotent.
    void TransitionToServing() noexcept;

    /// Move to kDraining and flip Health.Status to Draining.
    /// Idempotent. Returns true if this call performed the transition,
    /// false if drain had already been signalled.
    bool SignalDrain() noexcept;

    /// Move to kStopped. Called from the shutdown entry point after
    /// all subsystems have torn down. Idempotent.
    void MarkStopped() noexcept;

  private:
    std::atomic<State> state_{State::kLoaded};
    Health* health_;
};

}  // namespace osw

#endif  // OSW_CORE_LIFECYCLE_H_
