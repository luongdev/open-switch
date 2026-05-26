/*
 * include/osw/raii/fs_api.h
 *
 * Thin shim over the FreeSWITCH C API symbols that the RAII helpers
 * call. Two build modes:
 *
 *   - Production build (default): includes <switch.h> and aliases the
 *     `osw::raii::fs::*` function symbols to the real
 *     `switch_*` functions via thin inline trampolines. The trampolines
 *     compile down to direct calls at -O1+.
 *
 *   - Unit-test build (`-DOSW_TEST_FS_MOCK=1`): instead includes
 *     `fs_mock.h` which forward-declares the FreeSWITCH opaque types
 *     and exposes the `osw::raii::fs::*` symbols as function-pointer
 *     hooks that tests can override.
 *
 * This is the FS-mock test seam the W1 contract requires (§"include/osw/raii"
 * in W1-foundation.md). The choice — header-only function-pointer seam
 * keyed on a single macro — is documented in
 * `tests/unit/raii/README.md`.
 *
 * Memory-safety rationale: the RAII helpers in osw/raii take and
 * release locks / events / bugs / xml trees via the FS C API. Each
 * helper calls exactly one acquire function and exactly one release
 * function from this shim. By routing every call through this header,
 * we get:
 *
 *   1. A single place for `// FF-NNN cite` comments next to the FS API
 *      identifiers, making the cite discipline easier to audit.
 *   2. A test seam that does NOT require the production code to pull in
 *      gmock or to add per-helper function-pointer parameters to its
 *      constructors. The production .so contains no test plumbing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_FS_API_H_
#define OSW_RAII_FS_API_H_

#if defined(OSW_TEST_FS_MOCK)
#include "osw/raii/fs_mock.h"
#else

#include <switch.h>  // FreeSWITCH public API (header-only at module compile time)

#include <ctime>     // for time_t in MediaBugLease signature

namespace osw::raii::fs {

// --- Session lock / unlock (FF-016) ----------------------------------
//
// FF-016: switch_core_session_locate(uuid) returns a read-locked session
// (or NULL). The caller MUST switch_core_session_rwunlock(session) on
// every non-NULL return. The macro `switch_core_session_locate` expands
// to a perform_locate call with __FILE__/__FUNC__/__LINE__; we route
// through it so debug-rwlocks builds still get useful diagnostics.

inline switch_core_session_t* SessionLocate(const char* uuid) noexcept {
    return uuid ? switch_core_session_locate(uuid) : nullptr;
}

inline void SessionRwunlock(switch_core_session_t* session) noexcept {
    if (session) {
        switch_core_session_rwunlock(session);
    }
}

inline switch_channel_t* SessionGetChannel(switch_core_session_t* session) noexcept {
    return session ? switch_core_session_get_channel(session) : nullptr;
}

// --- Event create / destroy / fire (FF-012 adjacent) -----------------
//
// `switch_event_create(&ev, type)` returns SWITCH_STATUS_SUCCESS and
// writes a new event* into `*ev`, OR returns a failure status leaving
// *ev untouched. `switch_event_destroy(&ev)` is safe on NULL and sets
// *ev to NULL. `switch_event_fire(&ev)` transfers ownership to FS on
// success and sets *ev to NULL (see src/switch_event.c:391 in v1.10.12).

inline switch_status_t EventCreate(switch_event_t** out, switch_event_types_t type) noexcept {
    return switch_event_create(out, type);
}

inline void EventDestroy(switch_event_t** ev) noexcept {
    switch_event_destroy(ev);
}

inline switch_status_t EventFire(switch_event_t** ev) noexcept {
    return switch_event_fire(ev);
}

// --- Media bug add / remove ------------------------------------------
//
// `switch_core_media_bug_add(...)` allocates and inserts a bug into
// session->bugs (FF-007 covers insertion order). Returns SUCCESS with
// the bug ptr written to `*bug_out`, or a failure status. The remove
// counterpart `switch_core_media_bug_remove(session, &bug)` is the
// standard tear-down for bugs WE attached. See FF-002 for why we
// must NOT use `switch_core_media_bug_remove_callback` for FS-native
// bugs (the thread-id gate).

inline switch_status_t MediaBugAdd(switch_core_session_t* session,
                                   const char* name,
                                   const char* function,
                                   switch_media_bug_callback_t callback,
                                   void* user_data,
                                   time_t stop_time,
                                   uint32_t flags,
                                   switch_media_bug_t** bug_out) noexcept {
    return switch_core_media_bug_add(session, name, function, callback, user_data,
                                     stop_time, flags, bug_out);
}

inline switch_status_t MediaBugRemove(switch_core_session_t* session,
                                      switch_media_bug_t** bug) noexcept {
    return switch_core_media_bug_remove(session, bug);
}

// --- XML open_cfg / free (FF-015) ------------------------------------
//
// `switch_xml_open_cfg(path, &node, params)` returns the root XML* or
// NULL. The root is refcounted (FF-015). `switch_xml_free(NULL)` is
// safe.

inline switch_xml_t XmlOpenCfg(const char* file_path,
                               switch_xml_t* out_node,
                               switch_event_t* params) noexcept {
    return switch_xml_open_cfg(file_path, out_node, params);
}

inline void XmlFree(switch_xml_t xml) noexcept {
    switch_xml_free(xml);
}

}  // namespace osw::raii::fs

#endif  // !OSW_TEST_FS_MOCK

#endif  // OSW_RAII_FS_API_H_
