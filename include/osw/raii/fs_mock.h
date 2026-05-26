/*
 * include/osw/raii/fs_mock.h
 *
 * FreeSWITCH-API mock layer for unit tests of the RAII helpers.
 *
 * Activated by `-DOSW_TEST_FS_MOCK=1` on the test target only. The
 * production `mod_open_switch.so` is built WITHOUT this macro and
 * includes <switch.h> directly via `fs_api.h`.
 *
 * This header:
 *
 *   1. Forward-declares the FreeSWITCH opaque struct types that the
 *      RAII helpers reference (switch_core_session_t, switch_event_t,
 *      switch_media_bug_t, switch_xml_t). They are "incomplete types"
 *      — tests only ever hold pointers to them.
 *
 *   2. Redeclares the small subset of FS enums / typedefs the RAII
 *      helpers need (switch_status_t, switch_event_types_t,
 *      switch_media_bug_callback_t). Values match v1.10.12.
 *
 *   3. Declares the `osw::raii::fs::*` shim functions as function
 *      pointers, all defaulting to lambdas that increment counters
 *      visible to tests. Each test installs its own behaviour by
 *      assigning into the function-pointer slot.
 *
 *   4. Exposes a `MockReset()` helper that restores all hooks to
 *      defaults between tests.
 *
 * The mock is intentionally MINIMAL. It is not a full FS emulator and
 * cannot exercise anything beyond the RAII helpers' acquire/release
 * pairing. That's exactly the contract of the W1 RAII tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_FS_MOCK_H_
#define OSW_RAII_FS_MOCK_H_

#if !defined(OSW_TEST_FS_MOCK)
#error "fs_mock.h must only be included when OSW_TEST_FS_MOCK is defined"
#endif

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>

// --- FreeSWITCH types (opaque forward decls) -------------------------
//
// In v1.10.12 these are real structs (e.g. struct switch_core_session in
// src/include/switch_core_session.h). Tests only ever hold pointers, so
// they can stay incomplete here.

struct switch_core_session;
using switch_core_session_t = switch_core_session;

struct switch_channel;
using switch_channel_t = switch_channel;

struct switch_event;
using switch_event_t = switch_event;

struct switch_media_bug;
using switch_media_bug_t = switch_media_bug;

struct switch_xml;
using switch_xml_t = switch_xml*;  // FS typedefs switch_xml_t to a pointer

// --- FS status / enum values (subset, matched to v1.10.12) -----------
//
// switch_types.h:1031 in v1.10.12. We do NOT include all values — just
// the ones the RAII tests care about. Distinct from the real
// switch_status_t numeric values is harmless (mock-only).

using switch_status_t = int;
constexpr switch_status_t SWITCH_STATUS_SUCCESS = 0;
constexpr switch_status_t SWITCH_STATUS_FALSE   = 1;
constexpr switch_status_t SWITCH_STATUS_GENERR  = 8;

// switch_types.h:2179 in v1.10.12 — SWITCH_EVENT_CUSTOM is enum value 78
// at the time of v1.10.12; the exact value doesn't matter for mock tests,
// but we keep it distinguishable from 0.
using switch_event_types_t = int;
constexpr switch_event_types_t SWITCH_EVENT_CUSTOM = 78;

// Bug callback signature (matches src/include/switch_module_interfaces.h:54
// at v1.10.12). The mock never actually invokes a bug callback.
using switch_abc_type_t        = int;
using switch_bool_t            = int;
constexpr switch_bool_t SWITCH_FALSE = 0;
constexpr switch_bool_t SWITCH_TRUE  = 1;
using switch_media_bug_callback_t =
    switch_bool_t (*)(switch_media_bug_t*, void*, switch_abc_type_t);

// --- Mock state + hook pointers --------------------------------------

namespace osw::raii::fs {

struct MockState {
    // Acquire / release call counters. Tests assert on these.
    std::atomic<int> session_locate_calls{0};
    std::atomic<int> session_rwunlock_calls{0};
    std::atomic<int> event_create_calls{0};
    std::atomic<int> event_destroy_calls{0};
    std::atomic<int> event_fire_calls{0};
    std::atomic<int> media_bug_add_calls{0};
    std::atomic<int> media_bug_remove_calls{0};
    std::atomic<int> xml_open_cfg_calls{0};
    std::atomic<int> xml_free_calls{0};

    // Programmable return values for the next call. Set to non-default
    // to drive failure paths.
    switch_core_session_t* next_session       = nullptr;
    switch_channel_t*      next_channel       = nullptr;
    switch_event_t*        next_event         = nullptr;
    switch_status_t        next_event_create_status = SWITCH_STATUS_SUCCESS;
    switch_status_t        next_event_fire_status   = SWITCH_STATUS_SUCCESS;
    switch_media_bug_t*    next_bug           = nullptr;
    switch_status_t        next_bug_add_status      = SWITCH_STATUS_SUCCESS;
    switch_status_t        next_bug_remove_status   = SWITCH_STATUS_SUCCESS;
    switch_xml_t           next_xml_root      = nullptr;
};

// Single mutable instance accessed by both the (mock) shim functions and
// the tests. Tests should call `MockReset()` in their SetUp().
inline MockState& Mock() {
    static MockState s;
    return s;
}

inline void MockReset() {
    auto& m = Mock();
    m.session_locate_calls    = 0;
    m.session_rwunlock_calls  = 0;
    m.event_create_calls      = 0;
    m.event_destroy_calls     = 0;
    m.event_fire_calls        = 0;
    m.media_bug_add_calls     = 0;
    m.media_bug_remove_calls  = 0;
    m.xml_open_cfg_calls      = 0;
    m.xml_free_calls          = 0;
    m.next_session   = nullptr;
    m.next_channel   = nullptr;
    m.next_event     = nullptr;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_fire_status   = SWITCH_STATUS_SUCCESS;
    m.next_bug       = nullptr;
    m.next_bug_add_status      = SWITCH_STATUS_SUCCESS;
    m.next_bug_remove_status   = SWITCH_STATUS_SUCCESS;
    m.next_xml_root  = nullptr;
}

// --- Shim functions: SAME signatures as the production fs_api.h ------

inline switch_core_session_t* SessionLocate(const char* uuid) noexcept {
    if (!uuid) {
        return nullptr;
    }
    Mock().session_locate_calls.fetch_add(1, std::memory_order_relaxed);
    return Mock().next_session;
}

inline void SessionRwunlock(switch_core_session_t* session) noexcept {
    if (session) {
        Mock().session_rwunlock_calls.fetch_add(1, std::memory_order_relaxed);
    }
}

inline switch_channel_t* SessionGetChannel(switch_core_session_t* session) noexcept {
    return session ? Mock().next_channel : nullptr;
}

inline switch_status_t EventCreate(switch_event_t** out,
                                   switch_event_types_t /*type*/) noexcept {
    Mock().event_create_calls.fetch_add(1, std::memory_order_relaxed);
    if (Mock().next_event_create_status == SWITCH_STATUS_SUCCESS) {
        *out = Mock().next_event;
    } else {
        *out = nullptr;
    }
    return Mock().next_event_create_status;
}

inline void EventDestroy(switch_event_t** ev) noexcept {
    if (ev && *ev) {
        Mock().event_destroy_calls.fetch_add(1, std::memory_order_relaxed);
        *ev = nullptr;
    }
}

inline switch_status_t EventFire(switch_event_t** ev) noexcept {
    if (!ev || !*ev) {
        return SWITCH_STATUS_FALSE;
    }
    Mock().event_fire_calls.fetch_add(1, std::memory_order_relaxed);
    if (Mock().next_event_fire_status == SWITCH_STATUS_SUCCESS) {
        *ev = nullptr;  // mirrors v1.10.12 src/switch_event.c:391
    }
    return Mock().next_event_fire_status;
}

inline switch_status_t MediaBugAdd(switch_core_session_t* /*session*/,
                                   const char* /*name*/,
                                   const char* /*function*/,
                                   switch_media_bug_callback_t /*callback*/,
                                   void* /*user_data*/,
                                   time_t /*stop_time*/,
                                   uint32_t /*flags*/,
                                   switch_media_bug_t** bug_out) noexcept {
    Mock().media_bug_add_calls.fetch_add(1, std::memory_order_relaxed);
    if (Mock().next_bug_add_status == SWITCH_STATUS_SUCCESS) {
        *bug_out = Mock().next_bug;
    } else {
        *bug_out = nullptr;
    }
    return Mock().next_bug_add_status;
}

inline switch_status_t MediaBugRemove(switch_core_session_t* /*session*/,
                                      switch_media_bug_t** bug) noexcept {
    if (bug && *bug) {
        Mock().media_bug_remove_calls.fetch_add(1, std::memory_order_relaxed);
        if (Mock().next_bug_remove_status == SWITCH_STATUS_SUCCESS) {
            *bug = nullptr;
        }
        return Mock().next_bug_remove_status;
    }
    return SWITCH_STATUS_FALSE;
}

inline switch_xml_t XmlOpenCfg(const char* /*file_path*/,
                               switch_xml_t* out_node,
                               switch_event_t* /*params*/) noexcept {
    Mock().xml_open_cfg_calls.fetch_add(1, std::memory_order_relaxed);
    if (out_node) {
        *out_node = nullptr;
    }
    return Mock().next_xml_root;
}

inline void XmlFree(switch_xml_t xml) noexcept {
    if (xml) {
        Mock().xml_free_calls.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace osw::raii::fs

#endif  // OSW_RAII_FS_MOCK_H_
