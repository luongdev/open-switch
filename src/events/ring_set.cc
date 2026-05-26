/*
 * src/events/ring_set.cc
 *
 * Implementation of osw::events::RingSet — the bundle-of-three-Rings
 * type declared alongside Binder in osw/events/binder.h.
 *
 * Lives in osw_events (FS-agnostic) NOT osw_events_fs, because RingSet
 * holds three Ring values and nothing else — no <switch.h>, no FS APIs.
 * The Broadcaster (osw_events) references RingSet::Get() and
 * RingSet::CloseAll(), so these definitions MUST be in the same
 * library or the FS-agnostic test executables fail to link.
 *
 * The Binder methods (which DO need <switch.h>) stay in binder.cc
 * under osw_events_fs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <cstddef>

#include "osw/events/binder.h"
#include "osw/events/ring.h"
#include "osw/events/tier.h"

namespace osw::events {

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

}  // namespace osw::events
