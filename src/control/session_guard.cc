/*
 * src/control/session_guard.cc
 *
 * osw::control::SessionGuard factory implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/session_guard.h"

namespace osw::control {

// static
SessionGuard SessionGuard::Locate(const std::string& uuid) noexcept {
    if (uuid.empty()) {
        return SessionGuard{};
    }
    osw::SessionLock lock(uuid.c_str());
    if (!lock) {
        return SessionGuard{};
    }
    switch_channel_t* const ch = lock.channel();
    return SessionGuard{std::move(lock), ch};
}

}  // namespace osw::control
