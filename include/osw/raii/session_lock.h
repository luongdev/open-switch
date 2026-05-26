/*
 * include/osw/raii/session_lock.h
 *
 * osw::SessionLock — RAII pairing of
 *   switch_core_session_locate(uuid) / switch_core_session_rwunlock(session).
 *
 * Per FREESWITCH-FACTS FF-016: switch_core_session_locate returns a
 * read-locked session ptr (or NULL). Every non-NULL return MUST be
 * paired with exactly one switch_core_session_rwunlock(session). NULL
 * means "UUID not found" or "session tearing down" and needs no
 * release.
 *
 * The helper is move-only: copying a held read-lock would either
 * double-release (incorrect) or double-acquire (impossible without
 * locating again). Moves transfer ownership; reset() drops the lock
 * eagerly.
 *
 * Source: designs/memory-management.md §"osw::SessionLock" — this
 * implementation matches the verbatim helper text in that document.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_SESSION_LOCK_H_
#define OSW_RAII_SESSION_LOCK_H_

#include "osw/raii/fs_api.h"

namespace osw {

/// RAII guard for a FreeSWITCH session read-lock acquired via UUID.
///
/// Construction calls switch_core_session_locate(uuid). If the UUID
/// is not found or the lock cannot be acquired, the guard holds NULL
/// and operator bool() is false. Destruction releases the lock via
/// switch_core_session_rwunlock iff the guard is non-empty.
///
/// Cited FACTs:
/// - FF-016 — switch_core_session_locate read-lock contract.
class SessionLock {
  public:
    /// Constructs by locating `uuid`. If `uuid` is null or the UUID
    /// is unknown / tearing down, the resulting guard is empty.
    explicit SessionLock(const char* uuid) noexcept
        : session_(::osw::raii::fs::SessionLocate(uuid)) {}

    /// Releases the lock iff held. Non-throwing.
    ~SessionLock() noexcept { ::osw::raii::fs::SessionRwunlock(session_); }

    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    /// Transfers ownership of the held lock. The source becomes empty.
    SessionLock(SessionLock&& other) noexcept : session_(other.session_) {
        other.session_ = nullptr;
    }

    SessionLock& operator=(SessionLock&& other) noexcept {
        if (this != &other) {
            ::osw::raii::fs::SessionRwunlock(session_);
            session_ = other.session_;
            other.session_ = nullptr;
        }
        return *this;
    }

    /// Returns the underlying session pointer (non-owning view), or
    /// null if the guard is empty.
    [[nodiscard]] switch_core_session_t* get() const noexcept { return session_; }

    /// Returns the channel for the locked session, or null if empty.
    [[nodiscard]] switch_channel_t* channel() const noexcept {
        return ::osw::raii::fs::SessionGetChannel(session_);
    }

    /// True iff the guard holds a non-null session pointer.
    explicit operator bool() const noexcept { return session_ != nullptr; }

    /// Releases the lock eagerly. Safe to call on an empty guard;
    /// safe to call multiple times (subsequent calls are no-ops).
    void reset() noexcept {
        if (session_) {
            ::osw::raii::fs::SessionRwunlock(session_);
            session_ = nullptr;
        }
    }

  private:
    switch_core_session_t* session_;
};

}  // namespace osw

#endif  // OSW_RAII_SESSION_LOCK_H_
