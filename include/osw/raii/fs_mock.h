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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
constexpr switch_status_t SWITCH_STATUS_FALSE = 1;
constexpr switch_status_t SWITCH_STATUS_GENERR = 8;

// switch_types.h:2179 in v1.10.12 — SWITCH_EVENT_CUSTOM is enum value 78
// at the time of v1.10.12; the exact value doesn't matter for mock tests,
// but we keep it distinguishable from 0.
using switch_event_types_t = int;
constexpr switch_event_types_t SWITCH_EVENT_CUSTOM = 78;
constexpr switch_event_types_t SWITCH_EVENT_ALL = 100;

// switch_types.h:1031 — switch_stack_t. Only the two values our helpers
// pass through (BOTTOM, TOP) need to be distinguishable; SWITCH_STACK_BOTTOM
// is the FS-canonical "append to tail of header list".
using switch_stack_t = int;
constexpr switch_stack_t SWITCH_STACK_BOTTOM = 0;
constexpr switch_stack_t SWITCH_STACK_TOP = 1;

// switch_types.h:2146 — SWITCH_EVENT_GENERAL is a non-custom event type
// used as the container for channel variables in the ovars mechanism
// (FF-021). Its exact numeric value is not significant for tests.
constexpr switch_event_types_t SWITCH_EVENT_GENERAL = 64;

// switch_call_cause_t (switch_types.h:2182). Only the values the
// control-plane tests need to drive are declared here; the real enum
// has 60+ members. For test purposes distinct integer values are
// sufficient — tests do not pass them to real FS.
using switch_call_cause_t = int;
constexpr switch_call_cause_t SWITCH_CAUSE_NONE = 0;
constexpr switch_call_cause_t SWITCH_CAUSE_UNALLOCATED_NUMBER = 1;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_ROUTE_TRANSIT_NET = 2;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_ROUTE_DESTINATION = 3;
constexpr switch_call_cause_t SWITCH_CAUSE_CHANNEL_UNACCEPTABLE = 6;
constexpr switch_call_cause_t SWITCH_CAUSE_CALL_AWARDED_DELIVERED = 7;
constexpr switch_call_cause_t SWITCH_CAUSE_NORMAL_CLEARING = 16;
constexpr switch_call_cause_t SWITCH_CAUSE_USER_BUSY = 17;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_USER_RESPONSE = 18;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_ANSWER = 19;
constexpr switch_call_cause_t SWITCH_CAUSE_SUBSCRIBER_ABSENT = 20;
constexpr switch_call_cause_t SWITCH_CAUSE_CALL_REJECTED = 21;
constexpr switch_call_cause_t SWITCH_CAUSE_NUMBER_CHANGED = 22;
constexpr switch_call_cause_t SWITCH_CAUSE_REDIRECTION_TO_NEW_DESTINATION = 23;
constexpr switch_call_cause_t SWITCH_CAUSE_EXCHANGE_ROUTING_ERROR = 25;
constexpr switch_call_cause_t SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER = 27;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_NUMBER_FORMAT = 28;
constexpr switch_call_cause_t SWITCH_CAUSE_FACILITY_REJECTED = 29;
constexpr switch_call_cause_t SWITCH_CAUSE_RESPONSE_TO_STATUS_ENQUIRY = 30;
constexpr switch_call_cause_t SWITCH_CAUSE_NORMAL_UNSPECIFIED = 31;
constexpr switch_call_cause_t SWITCH_CAUSE_NORMAL_CIRCUIT_CONGESTION = 34;
constexpr switch_call_cause_t SWITCH_CAUSE_NETWORK_OUT_OF_ORDER = 38;
constexpr switch_call_cause_t SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE = 41;
constexpr switch_call_cause_t SWITCH_CAUSE_SWITCH_CONGESTION = 42;
constexpr switch_call_cause_t SWITCH_CAUSE_ACCESS_INFO_DISCARDED = 43;
constexpr switch_call_cause_t SWITCH_CAUSE_REQUESTED_CHAN_UNAVAIL = 44;
constexpr switch_call_cause_t SWITCH_CAUSE_PRE_EMPTED = 45;
constexpr switch_call_cause_t SWITCH_CAUSE_FACILITY_NOT_SUBSCRIBED = 50;
constexpr switch_call_cause_t SWITCH_CAUSE_OUTGOING_CALL_BARRED = 52;
constexpr switch_call_cause_t SWITCH_CAUSE_INCOMING_CALL_BARRED = 54;
constexpr switch_call_cause_t SWITCH_CAUSE_BEARERCAPABILITY_NOTAUTH = 57;
constexpr switch_call_cause_t SWITCH_CAUSE_BEARERCAPABILITY_NOTAVAIL = 58;
constexpr switch_call_cause_t SWITCH_CAUSE_SERVICE_UNAVAILABLE = 63;
constexpr switch_call_cause_t SWITCH_CAUSE_BEARERCAPABILITY_NOTIMPL = 65;
constexpr switch_call_cause_t SWITCH_CAUSE_CHAN_NOT_IMPLEMENTED = 66;
constexpr switch_call_cause_t SWITCH_CAUSE_FACILITY_NOT_IMPLEMENTED = 69;
constexpr switch_call_cause_t SWITCH_CAUSE_SERVICE_NOT_IMPLEMENTED = 79;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_CALL_REFERENCE = 81;
constexpr switch_call_cause_t SWITCH_CAUSE_INCOMPATIBLE_DESTINATION = 88;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_MSG_UNSPECIFIED = 95;
constexpr switch_call_cause_t SWITCH_CAUSE_MANDATORY_IE_MISSING = 96;
constexpr switch_call_cause_t SWITCH_CAUSE_MESSAGE_TYPE_NONEXIST = 97;
constexpr switch_call_cause_t SWITCH_CAUSE_WRONG_MESSAGE = 98;
constexpr switch_call_cause_t SWITCH_CAUSE_IE_NONEXIST = 99;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_IE_CONTENTS = 100;
constexpr switch_call_cause_t SWITCH_CAUSE_WRONG_CALL_STATE = 101;
constexpr switch_call_cause_t SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE = 102;
constexpr switch_call_cause_t SWITCH_CAUSE_MANDATORY_IE_LENGTH_ERROR = 103;
constexpr switch_call_cause_t SWITCH_CAUSE_PROTOCOL_ERROR = 111;
constexpr switch_call_cause_t SWITCH_CAUSE_INTERWORKING = 127;
constexpr switch_call_cause_t SWITCH_CAUSE_SUCCESS = 142;
constexpr switch_call_cause_t SWITCH_CAUSE_ORIGINATOR_CANCEL = 487;
constexpr switch_call_cause_t SWITCH_CAUSE_CRASH = 700;
constexpr switch_call_cause_t SWITCH_CAUSE_SYSTEM_SHUTDOWN = 701;
constexpr switch_call_cause_t SWITCH_CAUSE_LOSE_RACE = 702;
constexpr switch_call_cause_t SWITCH_CAUSE_MANAGER_REQUEST = 703;
constexpr switch_call_cause_t SWITCH_CAUSE_BLIND_TRANSFER = 800;
constexpr switch_call_cause_t SWITCH_CAUSE_ATTENDED_TRANSFER = 801;
constexpr switch_call_cause_t SWITCH_CAUSE_ALLOTTED_TIMEOUT = 802;
constexpr switch_call_cause_t SWITCH_CAUSE_USER_CHALLENGE = 803;
constexpr switch_call_cause_t SWITCH_CAUSE_MEDIA_TIMEOUT = 804;
constexpr switch_call_cause_t SWITCH_CAUSE_PICKED_OFF = 805;
constexpr switch_call_cause_t SWITCH_CAUSE_USER_NOT_REGISTERED = 806;
constexpr switch_call_cause_t SWITCH_CAUSE_PROGRESS_TIMEOUT = 807;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_GATEWAY = 808;
constexpr switch_call_cause_t SWITCH_CAUSE_GATEWAY_DOWN = 809;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_URL = 810;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_PROFILE = 811;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_PICKUP = 812;
constexpr switch_call_cause_t SWITCH_CAUSE_SRTP_READ_ERROR = 813;
constexpr switch_call_cause_t SWITCH_CAUSE_BOWOUT = 814;
constexpr switch_call_cause_t SWITCH_CAUSE_BUSY_EVERYWHERE = 815;
constexpr switch_call_cause_t SWITCH_CAUSE_DECLINE = 816;
constexpr switch_call_cause_t SWITCH_CAUSE_DOES_NOT_EXIST_ANYWHERE = 817;
constexpr switch_call_cause_t SWITCH_CAUSE_NOT_ACCEPTABLE = 818;
constexpr switch_call_cause_t SWITCH_CAUSE_UNWANTED = 819;
constexpr switch_call_cause_t SWITCH_CAUSE_NO_IDENTITY = 820;
constexpr switch_call_cause_t SWITCH_CAUSE_BAD_IDENTITY_INFO = 821;
constexpr switch_call_cause_t SWITCH_CAUSE_UNSUPPORTED_CERTIFICATE = 822;
constexpr switch_call_cause_t SWITCH_CAUSE_INVALID_IDENTITY = 823;
constexpr switch_call_cause_t SWITCH_CAUSE_STALE_DATE = 824;
constexpr switch_call_cause_t SWITCH_CAUSE_REJECT_ALL = 825;

// switch_originate_flag_t (switch_types.h:330). SOF_NONE = 0 is all
// we need in tests.
using switch_originate_flag_t = int;
constexpr switch_originate_flag_t SOF_NONE = 0;

// switch_channel_state_t (switch_types.h:1392). CS_HANGUP is the
// threshold used to detect a dead channel (channel_down_nosig check).
using switch_channel_state_t = int;
constexpr switch_channel_state_t CS_NEW = 0;
constexpr switch_channel_state_t CS_INIT = 1;
constexpr switch_channel_state_t CS_ROUTING = 2;
constexpr switch_channel_state_t CS_EXECUTE = 4;
constexpr switch_channel_state_t CS_HANGUP = 8;
constexpr switch_channel_state_t CS_DONE = 9;

// FF-018 callback typedef. The mock never invokes one — tests that exercise
// the bind path simply assert on captured registrations.
using switch_event_callback_t = void (*)(switch_event_t*);

// Bug callback signature (matches src/include/switch_module_interfaces.h:54
// at v1.10.12). The mock never actually invokes a bug callback.
using switch_abc_type_t = int;
using switch_bool_t = int;
constexpr switch_bool_t SWITCH_FALSE = 0;
constexpr switch_bool_t SWITCH_TRUE = 1;
using switch_media_bug_callback_t = switch_bool_t (*)(switch_media_bug_t*,
                                                      void*,
                                                      switch_abc_type_t);

// --- Mock state + hook pointers --------------------------------------

namespace osw::raii::fs {

struct MockState {
    // Acquire / release call counters. Tests assert on these.
    std::atomic<int> session_locate_calls{0};
    std::atomic<int> session_rwunlock_calls{0};
    std::atomic<int> event_create_calls{0};
    std::atomic<int> event_create_subclass_calls{0};
    std::atomic<int> event_add_header_calls{0};
    std::atomic<int> event_destroy_calls{0};
    std::atomic<int> event_fire_calls{0};
    std::atomic<int> event_bind_calls{0};
    std::atomic<int> event_unbind_calls{0};
    std::atomic<int> media_bug_add_calls{0};
    std::atomic<int> media_bug_remove_calls{0};
    std::atomic<int> xml_open_cfg_calls{0};
    std::atomic<int> xml_free_calls{0};
    // W3 control-plane counters (FF-021 + FF-022).
    std::atomic<int> originate_calls{0};
    std::atomic<int> channel_hangup_calls{0};

    // Programmable return values for the next call. Set to non-default
    // to drive failure paths.
    switch_core_session_t* next_session = nullptr;
    switch_channel_t* next_channel = nullptr;
    switch_event_t* next_event = nullptr;
    switch_status_t next_event_create_status = SWITCH_STATUS_SUCCESS;
    switch_status_t next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    switch_status_t next_event_fire_status = SWITCH_STATUS_SUCCESS;
    switch_status_t next_event_bind_status = SWITCH_STATUS_SUCCESS;
    switch_media_bug_t* next_bug = nullptr;
    switch_status_t next_bug_add_status = SWITCH_STATUS_SUCCESS;
    switch_status_t next_bug_remove_status = SWITCH_STATUS_SUCCESS;
    switch_xml_t next_xml_root = nullptr;
    // W3 originate / hangup programmable returns.
    switch_status_t next_originate_status = SWITCH_STATUS_SUCCESS;
    switch_core_session_t* next_originate_bleg = nullptr;
    switch_call_cause_t next_originate_cause = SWITCH_CAUSE_NORMAL_CLEARING;
    // UUID string for the next successfully originated bleg.
    std::string next_bleg_uuid;
    switch_channel_state_t next_channel_hangup_state = CS_HANGUP;

    // Captured artefacts for verification. Tests read these under
    // capture_mu (which is locked by the shim layer when writing).
    std::mutex capture_mu;

    // Captured OriginateSession invocations (W3 FF-021).
    struct CapturedOriginate {
        std::string dial_string;
        uint32_t timelimit_sec = 0;
        std::string cid_name;
        std::string cid_num;
    };
    std::vector<CapturedOriginate> originate_invocations;

    // Captured ChannelHangup invocations (W3 FF-022).
    struct CapturedHangup {
        switch_channel_t* channel = nullptr;
        switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
    };
    std::vector<CapturedHangup> hangup_invocations;

    // Per-event-pointer state, keyed on switch_event_t*. Populated by
    // EventCreateSubclass + EventAddHeaderString.
    struct CapturedEvent {
        switch_event_types_t type = 0;
        std::string subclass_name;
        std::vector<std::pair<std::string, std::string>> headers;
        bool fired = false;
    };
    std::unordered_map<switch_event_t*, CapturedEvent> events_by_ptr;

    // Captured bind/unbind registrations. The bind shim stores callback
    // + user_data; unbind matches on callback equality.
    struct CapturedBinding {
        std::string id;
        switch_event_types_t event = 0;
        std::string subclass_name;
        switch_event_callback_t callback = nullptr;
        void* user_data = nullptr;
        bool active = true;
    };
    std::vector<CapturedBinding> bindings;
};

// Single mutable instance accessed by both the (mock) shim functions and
// the tests. Tests should call `MockReset()` in their SetUp().
inline MockState& Mock() {
    static MockState s;
    return s;
}

inline void MockReset() {
    auto& m = Mock();
    m.session_locate_calls = 0;
    m.session_rwunlock_calls = 0;
    m.event_create_calls = 0;
    m.event_create_subclass_calls = 0;
    m.event_add_header_calls = 0;
    m.event_destroy_calls = 0;
    m.event_fire_calls = 0;
    m.event_bind_calls = 0;
    m.event_unbind_calls = 0;
    m.media_bug_add_calls = 0;
    m.media_bug_remove_calls = 0;
    m.xml_open_cfg_calls = 0;
    m.xml_free_calls = 0;
    m.next_session = nullptr;
    m.next_channel = nullptr;
    m.next_event = nullptr;
    m.next_event_create_status = SWITCH_STATUS_SUCCESS;
    m.next_event_create_subclass_status = SWITCH_STATUS_SUCCESS;
    m.next_event_fire_status = SWITCH_STATUS_SUCCESS;
    m.next_event_bind_status = SWITCH_STATUS_SUCCESS;
    m.next_bug = nullptr;
    m.next_bug_add_status = SWITCH_STATUS_SUCCESS;
    m.next_bug_remove_status = SWITCH_STATUS_SUCCESS;
    m.next_xml_root = nullptr;
    m.originate_calls = 0;
    m.channel_hangup_calls = 0;
    m.next_originate_status = SWITCH_STATUS_SUCCESS;
    m.next_originate_bleg = nullptr;
    m.next_originate_cause = SWITCH_CAUSE_NORMAL_CLEARING;
    m.next_bleg_uuid.clear();
    m.next_channel_hangup_state = CS_HANGUP;
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        m.events_by_ptr.clear();
        m.bindings.clear();
        m.originate_invocations.clear();
        m.hangup_invocations.clear();
    }
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

inline switch_status_t EventCreate(switch_event_t** out, switch_event_types_t type) noexcept {
    auto& m = Mock();
    m.event_create_calls.fetch_add(1, std::memory_order_relaxed);
    if (m.next_event_create_status == SWITCH_STATUS_SUCCESS) {
        *out = m.next_event;
        if (m.next_event) {
            std::lock_guard<std::mutex> g(m.capture_mu);
            auto& cap = m.events_by_ptr[m.next_event];
            cap.type = type;
        }
    } else {
        *out = nullptr;
    }
    return m.next_event_create_status;
}

inline switch_status_t EventCreateSubclass(switch_event_t** out,
                                           switch_event_types_t type,
                                           const char* subclass_name) noexcept {
    auto& m = Mock();
    m.event_create_subclass_calls.fetch_add(1, std::memory_order_relaxed);
    // FF-020: subclass_name with event_id != CUSTOM/CLONE returns GENERR.
    // We don't bother emulating the GENERR path here; tests set
    // next_event_create_subclass_status to drive failure.
    if (m.next_event_create_subclass_status == SWITCH_STATUS_SUCCESS) {
        *out = m.next_event;
        if (m.next_event) {
            std::lock_guard<std::mutex> g(m.capture_mu);
            auto& cap = m.events_by_ptr[m.next_event];
            cap.type = type;
            if (subclass_name) {
                cap.subclass_name = subclass_name;
                // FF-020: the FS call also adds "Event-Subclass" itself.
                cap.headers.emplace_back("Event-Subclass", subclass_name);
            }
        }
    } else {
        *out = nullptr;
    }
    return m.next_event_create_subclass_status;
}

inline switch_status_t EventAddHeaderString(switch_event_t* ev,
                                            switch_stack_t /*stack*/,
                                            const char* name,
                                            const char* value) noexcept {
    auto& m = Mock();
    m.event_add_header_calls.fetch_add(1, std::memory_order_relaxed);
    if (!ev || !name) {
        return SWITCH_STATUS_FALSE;
    }
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(ev);
    if (it == m.events_by_ptr.end()) {
        return SWITCH_STATUS_FALSE;
    }
    it->second.headers.emplace_back(name, value ? value : "");
    return SWITCH_STATUS_SUCCESS;
}

inline void EventDestroy(switch_event_t** ev) noexcept {
    if (ev && *ev) {
        auto& m = Mock();
        m.event_destroy_calls.fetch_add(1, std::memory_order_relaxed);
        // Per FS semantics: destroy frees headers + body. We keep the
        // capture for post-mortem assertions; tests can look at
        // events_by_ptr[ptr] after destruction.
        *ev = nullptr;
    }
}

inline switch_status_t EventFire(switch_event_t** ev) noexcept {
    if (!ev || !*ev) {
        return SWITCH_STATUS_FALSE;
    }
    auto& m = Mock();
    m.event_fire_calls.fetch_add(1, std::memory_order_relaxed);
    if (m.next_event_fire_status == SWITCH_STATUS_SUCCESS) {
        {
            std::lock_guard<std::mutex> g(m.capture_mu);
            auto it = m.events_by_ptr.find(*ev);
            if (it != m.events_by_ptr.end()) {
                it->second.fired = true;
            }
        }
        *ev = nullptr;  // mirrors v1.10.12 src/switch_event.c:391
    }
    return m.next_event_fire_status;
}

inline switch_status_t EventBind(const char* id,
                                 switch_event_types_t event,
                                 const char* subclass_name,
                                 switch_event_callback_t callback,
                                 void* user_data) noexcept {
    auto& m = Mock();
    m.event_bind_calls.fetch_add(1, std::memory_order_relaxed);
    if (m.next_event_bind_status != SWITCH_STATUS_SUCCESS) {
        return m.next_event_bind_status;
    }
    std::lock_guard<std::mutex> g(m.capture_mu);
    MockState::CapturedBinding b;
    b.id = id ? id : "";
    b.event = event;
    b.subclass_name = subclass_name ? subclass_name : "";
    b.callback = callback;
    b.user_data = user_data;
    b.active = true;
    m.bindings.push_back(std::move(b));
    return SWITCH_STATUS_SUCCESS;
}

inline switch_status_t EventUnbindCallback(switch_event_callback_t callback) noexcept {
    auto& m = Mock();
    m.event_unbind_calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(m.capture_mu);
    bool any = false;
    for (auto& b : m.bindings) {
        if (b.active && b.callback == callback) {
            b.active = false;
            any = true;
        }
    }
    return any ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

inline const char* EventGetHeader(switch_event_t* ev, const char* name) noexcept {
    if (!ev || !name) {
        return nullptr;
    }
    auto& m = Mock();
    std::lock_guard<std::mutex> g(m.capture_mu);
    auto it = m.events_by_ptr.find(ev);
    if (it == m.events_by_ptr.end()) {
        return nullptr;
    }
    for (const auto& [k, v] : it->second.headers) {
        if (k == name) {
            return v.c_str();
        }
    }
    return nullptr;
}

inline const char* EventGetBody(switch_event_t* ev) noexcept {
    // Bodies are not modelled in the mock; tests that need bodies set
    // the body via a custom header convention.
    return nullptr;
    (void)ev;
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

// --- switch_ivr_originate wrapper (FF-021) ---------------------------
//
// FF-021: V1 unattended originate (session == NULL). The real function
// sets *bleg to the new session (caller owns the rwlock) and *cause to
// the Q.850 result. The mock records the call and returns
// next_originate_status; if SUCCESS, writes next_originate_bleg into
// *bleg and next_originate_cause into *cause.

inline switch_status_t OriginateSession(switch_core_session_t* /*session*/,
                                        switch_core_session_t** bleg,
                                        switch_call_cause_t* cause,
                                        const char* bridgeto,
                                        uint32_t timelimit_sec,
                                        const char* cid_name_override,
                                        const char* cid_num_override,
                                        switch_event_t* /*ovars*/) noexcept {
    auto& m = Mock();
    m.originate_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        MockState::CapturedOriginate cap;
        cap.dial_string = bridgeto ? bridgeto : "";
        cap.timelimit_sec = timelimit_sec;
        cap.cid_name = cid_name_override ? cid_name_override : "";
        cap.cid_num = cid_num_override ? cid_num_override : "";
        m.originate_invocations.push_back(std::move(cap));
    }
    if (cause != nullptr) {
        *cause = m.next_originate_cause;
    }
    if (m.next_originate_status == SWITCH_STATUS_SUCCESS) {
        if (bleg != nullptr) {
            *bleg = m.next_originate_bleg;
        }
    } else {
        if (bleg != nullptr) {
            *bleg = nullptr;
        }
    }
    return m.next_originate_status;
}

// --- switch_channel_hangup wrapper (FF-022) --------------------------
//
// FF-022: switch_channel_hangup is a macro that expands to
// switch_channel_perform_hangup. It is idempotent (second call on an
// already-hung-up channel returns CS_HANGUP without side effects). The
// mock records the call and returns next_channel_hangup_state.

inline switch_channel_state_t ChannelHangup(switch_channel_t* channel,
                                            switch_call_cause_t cause) noexcept {
    auto& m = Mock();
    m.channel_hangup_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(m.capture_mu);
        MockState::CapturedHangup cap;
        cap.channel = channel;
        cap.cause = cause;
        m.hangup_invocations.push_back(cap);
    }
    return m.next_channel_hangup_state;
}

// --- switch_core_session_get_uuid wrapper ----------------------------
//
// Used by the Originate handler to read the new bleg's UUID after a
// successful switch_ivr_originate call. The mock returns the value
// of next_bleg_uuid (set by the test); returns nullptr if empty.

inline const char* SessionGetUuid(switch_core_session_t* session) noexcept {
    if (session == nullptr) {
        return nullptr;
    }
    const auto& uuid = Mock().next_bleg_uuid;
    return uuid.empty() ? nullptr : uuid.c_str();
}

}  // namespace osw::raii::fs

#endif  // OSW_RAII_FS_MOCK_H_
