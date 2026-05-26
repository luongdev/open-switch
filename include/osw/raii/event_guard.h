/*
 * include/osw/raii/event_guard.h
 *
 * osw::EventGuard — RAII pairing of
 *   switch_event_create(&ev, type) / switch_event_destroy(&ev)
 * with a fire() that transfers ownership to FreeSWITCH.
 *
 * Semantics (from FREESWITCH-FACTS):
 *   - switch_event_create returns SWITCH_STATUS_SUCCESS and writes a
 *     new event* to the out-param, or returns a failure status (e.g.
 *     SWITCH_STATUS_GENERR for invalid type+subclass combos) leaving
 *     the out-param NULL.
 *   - switch_event_destroy(&ev) is safe on NULL and sets the caller's
 *     ev to NULL.
 *   - switch_event_fire(&ev) transfers ownership to FS and sets the
 *     caller's ev to NULL on success (verified in v1.10.12
 *     src/switch_event.c:391, switch_event_queue_dispatch_event).
 *     On failure (system shutting down), FS internally destroys the
 *     event and the caller's ev is still nulled.
 *
 * Why move-only: copying would require duplicating the underlying
 * event headers, which is a different operation (switch_event_dup
 * exists for that purpose). The guard owns at most one event at a
 * time.
 *
 * Why a private ctor + adopt() factory: the ergonomic ctor
 * (`EventGuard ev(SWITCH_EVENT_CUSTOM)`) calls switch_event_create
 * synchronously. For the rare case where the caller already has an
 * event* from another FS API (e.g. a clone), they use
 * `EventGuard::adopt(ev)` to take ownership without re-creating.
 *
 * Source: designs/memory-management.md §"osw::EventGuard" — this
 * implementation matches the verbatim helper text in that document.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_EVENT_GUARD_H_
#define OSW_RAII_EVENT_GUARD_H_

#include "osw/raii/fs_api.h"

namespace osw {

/// RAII guard for a FreeSWITCH event*.
///
/// On construction, allocates a new event of the requested type. On
/// destruction, destroys the event iff it has not been fire()d (or
/// release()d) yet. fire() transfers ownership to FS.
///
/// Cited FACTs:
/// - FF-017: switch_event_create / _destroy / _fire ownership
///   semantics, including the always-null-on-return-from-fire
///   property this guard relies on.
class EventGuard {
  public:
    /// Creates a new event of the given type. On allocation failure
    /// the guard holds null and operator bool() is false.
    explicit EventGuard(switch_event_types_t type) noexcept : event_(nullptr) {
        if (::osw::raii::fs::EventCreate(&event_, type) != SWITCH_STATUS_SUCCESS) {
            event_ = nullptr;
        }
    }

    /// Convenience: create a CUSTOM event (the most common case for
    /// our internal emissions).
    EventGuard() noexcept : EventGuard(SWITCH_EVENT_CUSTOM) {}

    /// Tag type for the adopt() factory; lets us disambiguate from
    /// the public ctors without falling back to a private ctor + friend.
    struct adopt_t {};
    static constexpr adopt_t adopt_tag{};

    /// Takes ownership of an existing event* without re-allocating.
    /// Useful when an FS API hands us an event (e.g., a clone) and
    /// we want RAII cleanup. Prefer the static `adopt(ev)` factory
    /// for readability.
    EventGuard(adopt_t, switch_event_t* existing) noexcept : event_(existing) {}

    /// Factory wrapper around `EventGuard(adopt_tag, ev)`.
    static EventGuard adopt(switch_event_t* existing) noexcept {
        return EventGuard(adopt_tag, existing);
    }

    ~EventGuard() noexcept {
        if (event_) {
            ::osw::raii::fs::EventDestroy(&event_);
        }
    }

    EventGuard(const EventGuard&) = delete;
    EventGuard& operator=(const EventGuard&) = delete;

    EventGuard(EventGuard&& other) noexcept : event_(other.event_) { other.event_ = nullptr; }

    EventGuard& operator=(EventGuard&& other) noexcept {
        if (this != &other) {
            if (event_) {
                ::osw::raii::fs::EventDestroy(&event_);
            }
            event_ = other.event_;
            other.event_ = nullptr;
        }
        return *this;
    }

    /// Non-owning view of the underlying event*. null if empty.
    [[nodiscard]] switch_event_t* get() const noexcept { return event_; }

    /// True iff the guard owns an event.
    explicit operator bool() const noexcept { return event_ != nullptr; }

    /// Fire the event. Ownership transfers to FS on success; the guard
    /// becomes empty. Returns the underlying switch_event_fire status.
    /// Safe to call on an empty guard (returns SWITCH_STATUS_FALSE).
    switch_status_t fire() noexcept {
        if (!event_) {
            return SWITCH_STATUS_FALSE;
        }
        // switch_event_fire nulls the caller's ev on success; on failure
        // it still destroys the event (system shutting down path).
        // Either way, after this call we should treat our slot as empty.
        switch_status_t s = ::osw::raii::fs::EventFire(&event_);
        event_ = nullptr;
        return s;
    }

    /// Release ownership without destroying or firing. Caller is then
    /// responsible for the returned ptr's lifetime.
    [[nodiscard]] switch_event_t* release() noexcept {
        switch_event_t* e = event_;
        event_ = nullptr;
        return e;
    }

  private:
    switch_event_t* event_;
};

}  // namespace osw

#endif  // OSW_RAII_EVENT_GUARD_H_
