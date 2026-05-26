/*
 * src/core/lifecycle.cc
 *
 * Implements osw::Lifecycle. FS-agnostic; no <switch.h> required.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/lifecycle.h"

#include "osw/observability/health.h"

namespace osw {

void Lifecycle::TransitionToServing() noexcept {
    State expected = State::kLoaded;
    state_.compare_exchange_strong(expected, State::kServing,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire);
    // Idempotent: if we were already Serving, the CAS fails and we
    // leave state untouched. If we were already Draining or Stopped,
    // we DO NOT move backwards; the CAS still fails.
    if (health_) {
        // Only set kServing if Health isn't already in a stricter state.
        const auto current = health_->GetStatus();
        if (current == Health::Status::kUnspecified ||
            current == Health::Status::kServing) {
            health_->SetStatus(Health::Status::kServing);
        }
    }
}

bool Lifecycle::SignalDrain() noexcept {
    // Allow transition from any earlier state to Draining; reject if
    // already at Stopped.
    State expected = state_.load(std::memory_order_acquire);
    while (true) {
        if (expected == State::kDraining || expected == State::kStopped) {
            return false;
        }
        if (state_.compare_exchange_weak(expected, State::kDraining,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            break;
        }
        // Loop with refreshed `expected`.
    }
    if (health_) {
        health_->SetStatus(Health::Status::kDraining);
    }
    return true;
}

void Lifecycle::MarkStopped() noexcept {
    state_.store(State::kStopped, std::memory_order_release);
    if (health_) {
        health_->SetStatus(Health::Status::kNotServing);
    }
}

}  // namespace osw
