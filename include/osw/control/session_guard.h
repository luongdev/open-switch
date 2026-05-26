/*
 * include/osw/control/session_guard.h
 *
 * osw::control::SessionGuard — RAII wrapper around osw::SessionLock
 * that additionally extracts and caches
 * switch_core_session_get_channel(session) so handlers do not repeat
 * that lookup.
 *
 * Move-only (like SessionLock). Three states:
 *   - Empty:   !Valid(), Channel() == nullptr, get() == nullptr.
 *   - Locked:  Valid() == true, Channel() returns the channel ptr.
 *   - Released: after dtor or explicit Reset(), same as empty.
 *
 * Use:
 *   auto guard = osw::control::SessionGuard::Locate(uuid);
 *   if (!guard.Valid()) {
 *       // UUID unknown / session tearing down
 *   }
 *   switch_channel_t* ch = guard.Channel();
 *
 * Lock-order note: SessionGuard acquires only the session read-lock
 * (via switch_core_session_locate / SessionLock). No other mutex is
 * acquired. Handlers that need to perform multiple locked operations
 * must not hold two SessionGuards simultaneously unless they have
 * established a stable UUID-order to prevent deadlock (Track A
 * handlers never need two guards concurrently, so this is not an
 * issue for V1).
 *
 * Cited FACTs:
 *   - FF-016 — switch_core_session_locate read-lock contract.
 *   - FF-022 — switch_channel_hangup caller-lock requirement (caller
 *     must hold the session read-lock while calling channel_hangup).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_SESSION_GUARD_H_
#define OSW_CONTROL_SESSION_GUARD_H_

#include <string>

#include "osw/raii/session_lock.h"

namespace osw::control {

/// RAII session guard for use by control-plane handlers.
///
/// Extends osw::SessionLock with a cached channel pointer and a
/// named factory method for clarity at call sites.
class SessionGuard {
  public:
    /// Locate a session by UUID and return a guard.
    ///
    /// If `uuid` is empty, or the UUID is not found, the returned
    /// guard is empty (Valid() == false). Never throws.
    [[nodiscard]] static SessionGuard Locate(const std::string& uuid) noexcept;

    /// Constructs an empty (invalid) guard.
    SessionGuard() noexcept : lock_(nullptr), channel_(nullptr) {}

    ~SessionGuard() noexcept = default;

    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

    SessionGuard(SessionGuard&& other) noexcept
        : lock_(std::move(other.lock_)), channel_(other.channel_) {
        other.channel_ = nullptr;
    }

    SessionGuard& operator=(SessionGuard&& other) noexcept {
        if (this != &other) {
            lock_ = std::move(other.lock_);
            channel_ = other.channel_;
            other.channel_ = nullptr;
        }
        return *this;
    }

    /// True iff the guard holds a non-null session (locate succeeded).
    [[nodiscard]] bool Valid() const noexcept { return static_cast<bool>(lock_); }

    /// The underlying session pointer (non-owning). Null if !Valid().
    [[nodiscard]] switch_core_session_t* get() const noexcept { return lock_.get(); }

    /// The channel pointer cached at construction. Null if !Valid().
    [[nodiscard]] switch_channel_t* Channel() const noexcept { return channel_; }

    /// Release the session read-lock eagerly. Idempotent. After Reset()
    /// Valid() == false and Channel() == nullptr.
    void Reset() noexcept {
        lock_.reset();
        channel_ = nullptr;
    }

  private:
    /// Private constructor used by Locate().
    explicit SessionGuard(osw::SessionLock lock, switch_channel_t* channel) noexcept
        : lock_(std::move(lock)), channel_(channel) {}

    osw::SessionLock lock_;
    switch_channel_t* channel_ = nullptr;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_SESSION_GUARD_H_
