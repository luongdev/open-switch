/*
 * include/osw/raii/media_bug_lease.h
 *
 * osw::MediaBugLease — RAII pairing of
 *   switch_core_media_bug_add / switch_core_media_bug_remove.
 *
 * Per FREESWITCH-FACTS:
 *   - FF-007: switch_core_media_bug_add inserts the new bug at the
 *     head of session->bugs if SMBF_FIRST is set, or appends to the
 *     tail otherwise. There is no numeric priority byte.
 *   - FF-011: SWITCH_EVENT_MEDIA_BUG_START fires when a bug is
 *     attached. Our security guard (W4.5) subscribes to this for
 *     audit; not a concern for the RAII helper itself.
 *   - FF-002 / FF-003 explain why we cannot use
 *     switch_core_media_bug_remove_callback to remove bugs we did
 *     NOT attach (thread-id gate + static eavesdrop_callback). The
 *     RAII helper only removes bugs WE attached, so we use the
 *     ptr-based switch_core_media_bug_remove which has no thread
 *     restriction.
 *
 * W1 ships this helper but adds zero MediaBugLease instances. The
 * helper is required so that W4 (media plane) can use it from day
 * one without re-inventing the RAII.
 *
 * Source: designs/memory-management.md §"osw::MediaBugLease" — this
 * implementation matches the verbatim helper text in that document.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_MEDIA_BUG_LEASE_H_
#define OSW_RAII_MEDIA_BUG_LEASE_H_

#include "osw/raii/fs_api.h"

namespace osw {

/// RAII lease over a FreeSWITCH media bug.
///
/// Construction calls switch_core_media_bug_add. If the add fails,
/// the lease holds null and operator bool() is false. Destruction
/// calls switch_core_media_bug_remove iff the lease holds a bug.
///
/// The lease assumes the caller-supplied `user_data` ptr remains
/// valid for the lease's lifetime — typically `user_data` is an
/// instance of a bug-state struct owned by the same class that owns
/// the lease. C++ destruction order makes this safe when the lease
/// is the LAST member of the owning class.
///
/// Cited FACTs:
/// - FF-007 — bug insertion order.
class MediaBugLease {
 public:
    /// Attaches a media bug. `function` is the FS-internal name used
    /// for switch_core_media_bug_count() filtering (FF-008) and for
    /// the Media-Bug-Function header on SWITCH_EVENT_MEDIA_BUG_START
    /// (FF-011).
    MediaBugLease(switch_core_session_t* session,
                  const char* name,
                  const char* function,
                  switch_media_bug_callback_t callback,
                  void* user_data,
                  time_t stop_time,
                  uint32_t flags) noexcept
        : session_(session), bug_(nullptr) {
        if (session) {
            const switch_status_t s = ::osw::raii::fs::MediaBugAdd(
                session, name, function, callback, user_data,
                stop_time, flags, &bug_);
            if (s != SWITCH_STATUS_SUCCESS) {
                bug_ = nullptr;
            }
        }
    }

    ~MediaBugLease() noexcept { remove(); }

    MediaBugLease(const MediaBugLease&)            = delete;
    MediaBugLease& operator=(const MediaBugLease&) = delete;

    MediaBugLease(MediaBugLease&& other) noexcept
        : session_(other.session_), bug_(other.bug_) {
        other.session_ = nullptr;
        other.bug_     = nullptr;
    }

    MediaBugLease& operator=(MediaBugLease&& other) noexcept {
        if (this != &other) {
            remove();
            session_       = other.session_;
            bug_           = other.bug_;
            other.session_ = nullptr;
            other.bug_     = nullptr;
        }
        return *this;
    }

    /// Non-owning view of the underlying bug ptr. null if empty.
    [[nodiscard]] switch_media_bug_t* get() const noexcept { return bug_; }

    /// True iff the lease holds an attached bug.
    explicit operator bool() const noexcept { return bug_ != nullptr; }

    /// Removes the bug eagerly. Safe to call multiple times.
    void remove() noexcept {
        if (bug_ && session_) {
            ::osw::raii::fs::MediaBugRemove(session_, &bug_);
            bug_ = nullptr;
        }
    }

 private:
    switch_core_session_t* session_;
    switch_media_bug_t*    bug_;
};

}  // namespace osw

#endif  // OSW_RAII_MEDIA_BUG_LEASE_H_
