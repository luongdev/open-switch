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

#include <ctime>  // for time_t in MediaBugLease signature

#include <switch.h>  // FreeSWITCH public API (header-only at module compile time)

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

// --- Event create-subclass + header add (FF-020) ---------------------
//
// FF-020 — switch_event_create_subclass is the canonical entry point
// for CUSTOM events that carry a subclass_name. The macro expands to
// switch_event_create_subclass_detailed which (a) refuses any event_id
// other than CUSTOM/CLONE when subclass_name is non-NULL, (b) DUP's the
// subclass into (*event)->subclass_name, and (c) adds an
// "Event-Subclass" header carrying the same string.
//
// switch_event_add_header_string DUP's the value into the event's own
// allocation; the caller does NOT need to keep the source buffer alive
// past the call. The header name is also DUP'd internally.

inline switch_status_t EventCreateSubclass(switch_event_t** out,
                                           switch_event_types_t type,
                                           const char* subclass_name) noexcept {
    return switch_event_create_subclass(out, type, subclass_name);
}

inline switch_status_t EventAddHeaderString(switch_event_t* ev,
                                            switch_stack_t stack,
                                            const char* name,
                                            const char* value) noexcept {
    return switch_event_add_header_string(ev, stack, name, value);
}

// --- Event bind / unbind (FF-018) ------------------------------------
//
// FF-018 — switch_event_bind registers `callback` for `event` (with
// optional `subclass_name` filter, NULL = match all subclasses).
// switch_event_unbind_callback removes every registration whose
// `callback` matches under the FS rwlock; after it returns no further
// dispatch will invoke the callback.

inline switch_status_t EventBind(const char* id,
                                 switch_event_types_t event,
                                 const char* subclass_name,
                                 switch_event_callback_t callback,
                                 void* user_data) noexcept {
    return switch_event_bind(id, event, subclass_name, callback, user_data);
}

inline switch_status_t EventUnbindCallback(switch_event_callback_t callback) noexcept {
    return switch_event_unbind_callback(callback);
}

// --- Event-header read (FF-019) --------------------------------------
//
// FF-019 — switch_event_get_header returns an FS-owned char* whose
// lifetime is the event's. Caller MUST NOT free or retain past the
// callback's return. switch_event_get_body has identical ownership.

inline const char* EventGetHeader(switch_event_t* ev, const char* name) noexcept {
    return switch_event_get_header(ev, name);
}

inline const char* EventGetBody(switch_event_t* ev) noexcept {
    return switch_event_get_body(ev);
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
    return switch_core_media_bug_add(
        session, name, function, callback, user_data, stop_time, flags, bug_out);
}

inline switch_status_t MediaBugRemove(switch_core_session_t* session,
                                      switch_media_bug_t** bug) noexcept {
    return switch_core_media_bug_remove(session, bug);
}

inline switch_status_t MediaBugRemoveCallback(switch_core_session_t* session,
                                              switch_media_bug_callback_t callback) noexcept {
    return switch_core_media_bug_remove_callback(session, callback);
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

// --- Session UUID (helper for post-originate UUID retrieval) ---------
//
// switch_core_session_get_uuid returns the UUID string for a session.
// The returned pointer is owned by the session (never freed by caller).
// Lifetime: valid as long as the session object exists and the caller
// holds the read-lock (FF-016).

inline const char* SessionGetUuid(switch_core_session_t* session) noexcept {
    return session ? switch_core_session_get_uuid(session) : nullptr;
}

// --- switch_ivr_originate (FF-021) -----------------------------------
//
// FF-021: thin inline wrapper for the unattended (V1) originate path.
//
//   switch_ivr_originate(
//       NULL,       // session — NULL for unattended originate
//       &bleg,      // output: new session ptr (caller rwlock owner)
//       &cause,     // output: Q.850 cause
//       bridgeto,   // dial string
//       timelimit_sec,
//       NULL,       // state handler table — NULL OK
//       cid_name,   // cid name override
//       cid_num,    // cid number override
//       NULL,       // caller_profile_override — NULL OK
//       ovars,      // channel variable event (ownership transferred)
//       SOF_NONE,   // originate flags
//       NULL,       // cancel_cause — NULL OK
//       NULL);      // dial handle — NULL OK
//
// On SUCCESS: *bleg is set to the new session; the caller owns the
// rwlock and MUST call switch_core_session_rwunlock(*bleg) once done.
// *cause is set to SWITCH_CAUSE_SUCCESS (142) or the actual cause if
// answered with a non-success code.
// On FAILURE: *bleg is NULL (no rwlock acquired); *cause carries the
// Q.850 reason (e.g. SWITCH_CAUSE_USER_BUSY, SWITCH_CAUSE_NO_ANSWER).
// The ovars event is CONSUMED regardless of result — FS destroys it
// internally. Callers MUST NOT call switch_event_destroy on ovars
// after this call.

inline switch_status_t OriginateSession(switch_core_session_t* /*session*/,
                                        switch_core_session_t** bleg,
                                        switch_call_cause_t* cause,
                                        const char* bridgeto,
                                        uint32_t timelimit_sec,
                                        const char* cid_name_override,
                                        const char* cid_num_override,
                                        switch_event_t* ovars) noexcept {
    return switch_ivr_originate(nullptr,
                                bleg,
                                cause,
                                bridgeto,
                                timelimit_sec,
                                nullptr,
                                cid_name_override,
                                cid_num_override,
                                nullptr,
                                ovars,
                                SOF_NONE,
                                nullptr,
                                nullptr);
}

// --- switch_channel_hangup (FF-022) ----------------------------------
//
// FF-022: switch_channel_hangup is a macro that expands to
// switch_channel_perform_hangup (src/include/switch_channel.h:589).
// Idempotent: a second call on an already-hung-up channel (state >=
// CS_HANGUP) returns CS_HANGUP without further state transitions
// because the OCF_HANGUP flag is already set (verified in
// src/switch_channel.c:3380-3394).
// Caller MUST hold the session read-lock (FF-016, FF-022).
// Returns the resulting channel state.

inline switch_channel_state_t ChannelHangup(switch_channel_t* channel,
                                            switch_call_cause_t cause) noexcept {
    return switch_channel_hangup(channel, cause);
}

// W3 Track B — Bridge / Execute / BlindTransfer (FF-023..025) ---------

// --- switch_channel_get_state ----------------------------------------
//
// Returns the current call state of a channel. Used by Bridge to
// validate that both parties are in CS_ROUTING or CS_EXECUTE before
// attempting switch_ivr_uuid_bridge (FF-023).
// Caller MUST hold the session read-lock while calling (FF-016).

inline switch_channel_state_t ChannelGetState(switch_channel_t* channel) noexcept {
    return switch_channel_get_state(channel);
}

// --- switch_ivr_uuid_bridge (FF-023) ---------------------------------
//
// FF-023: bridge two live sessions identified by UUID. The helper
// locates both sessions internally; the handler pre-locks both via
// SessionGuards in lexicographic UUID order (lower UUID first) before
// calling this function, then releases both guards after it returns.
// Cited header: /usr/local/include/switch_ivr.h:617 (v1.10.12).

inline switch_status_t UuidBridge(const char* originator_uuid,
                                  const char* originatee_uuid) noexcept {
    return switch_ivr_uuid_bridge(originator_uuid, originatee_uuid);
}

// --- switch_core_session_execute_application (FF-024) ----------------
//
// FF-024: execute a dialplan app synchronously on a session. The macro
// switch_core_session_execute_application expands to
// switch_core_session_execute_application_get_flags with flags=NULL.
// The call blocks the calling thread until the app returns.
// Caller MUST hold the session read-lock across the entire call
// (FF-016). Cited header: /usr/local/include/switch_core.h:1129
// (v1.10.12).

inline switch_status_t ExecuteApplication(switch_core_session_t* session,
                                          const char* app,
                                          const char* arg) noexcept {
    return switch_core_session_execute_application(session, app, arg);
}

// --- switch_ivr_session_transfer (FF-025) ----------------------------
//
// FF-025: transfer a session to a new dialplan extension. `dialplan`
// and `context` are OPTIONAL (NULL means FS uses defaults: "XML" and
// the channel's current context, respectively). `extension` is
// REQUIRED and must be non-NULL. Cited header:
// /usr/local/include/switch_ivr.h:585 (v1.10.12).

inline switch_status_t SessionTransfer(switch_core_session_t* session,
                                       const char* extension,
                                       const char* dialplan,
                                       const char* context) noexcept {
    return switch_ivr_session_transfer(session, extension, dialplan, context);
}

// W3 Track C — SetVariables / Hold / Unhold (FF-026..027) -------------

// --- switch_channel_set_variable (FF-026) ----------------------------
//
// FF-026: switch_channel_set_variable is a macro that expands to
// switch_channel_set_variable_var_check(..., SWITCH_TRUE).
// FS copies both name and value into the channel's APR pool. Caller's
// buffers can be freed immediately after. Safe from any thread that
// holds the session read-lock (FF-016).
// Returns SWITCH_STATUS_SUCCESS on success, SWITCH_STATUS_FALSE if the
// channel is null or var_check fails.

inline switch_status_t ChannelSetVariable(switch_channel_t* channel,
                                          const char* name,
                                          const char* value) noexcept {
    return switch_channel_set_variable(channel, name, value);
}

// --- switch_channel_test_flag (FF-027) --------------------------------
//
// FF-027: switch_channel_test_flag returns non-zero if `flag` is set.
// Used to gate Hold (CF_ANSWERED) and Unhold (CF_HOLD) operations.
// Caller MUST hold the session read-lock (FF-016).
//
// NOTE: Track B also exposes a `ChannelGetState` wrapper above; ordering
// is fine since both are inline and free-standing.

inline uint32_t ChannelTestFlag(switch_channel_t* channel, switch_channel_flag_t flag) noexcept {
    return switch_channel_test_flag(channel, flag);
}

// --- switch_channel_set_flag (FF-037) --------------------------------
//
// FF-037: CF_BREAK asks blocking IVR media loops such as
// switch_ivr_play_file to break promptly at their next tick. The
// silence-driver hotfix uses this to stop the module-owned
// silence_stream://-1 driver when the last WRITE_REPLACE bug detaches.
// Caller MUST hold the session read-lock (FF-016).

inline void ChannelSetFlag(switch_channel_t* channel, switch_channel_flag_t flag) noexcept {
    if (channel) {
        switch_channel_set_flag(channel, flag);
    }
}

// --- switch_ivr_hold_uuid (FF-027) ------------------------------------
//
// FF-027: switch_ivr_hold_uuid(uuid, message, moh).
// `message` is a display string / MoH class name (NULL = FS default).
// `moh = SWITCH_TRUE` instructs FS to play music-on-hold.
// Returns SWITCH_STATUS_SUCCESS when FS accepts the request.

inline switch_status_t HoldUuid(const char* uuid, const char* message, switch_bool_t moh) noexcept {
    return switch_ivr_hold_uuid(uuid, message, moh);
}

// --- switch_ivr_unhold_uuid (FF-027) ----------------------------------
//
// FF-027: switch_ivr_unhold_uuid(uuid).
// Returns SWITCH_STATUS_SUCCESS when FS accepts the request.

inline switch_status_t UnholdUuid(const char* uuid) noexcept {
    return switch_ivr_unhold_uuid(uuid);
}

// --- switch_ivr_play_file (FF-037) ------------------------------------
//
// Drives a session's write side by playing an IVR media source. Used by
// W6.6 to play "silence_stream://-1" until CF_BREAK is set. The wrapper
// owns the zeroed switch_input_args_t so callers do not need to expose
// FS-only struct details in their public headers or mock tests.

inline switch_status_t IvrPlayFile(switch_core_session_t* session, const char* file) noexcept {
    switch_input_args_t args{};
    return switch_ivr_play_file(session, nullptr, file, &args);
}

// --- W6C media bug frame access (switch_core.h:322/336/342) -------------
//
// switch_core_media_bug_get_write_replace_frame — returns the frame the
//   FS write-replace callback should overwrite. Called with the bug active.
// switch_core_media_bug_set_write_replace_frame — installs the replacement
//   frame pointer (after writing samples into it).
// switch_core_media_bug_get_read_replace_frame — returns the read frame
//   for STT / read-stream bugs (read-only access).

inline switch_frame_t* MediaBugGetWriteReplaceFrame(switch_media_bug_t* bug) noexcept {
    return switch_core_media_bug_get_write_replace_frame(bug);
}

inline void MediaBugSetWriteReplaceFrame(switch_media_bug_t* bug, switch_frame_t* frame) noexcept {
    switch_core_media_bug_set_write_replace_frame(bug, frame);
}

inline switch_frame_t* MediaBugGetReadReplaceFrame(switch_media_bug_t* bug) noexcept {
    return switch_core_media_bug_get_read_replace_frame(bug);
}

}  // namespace osw::raii::fs

#endif  // !OSW_TEST_FS_MOCK

#endif  // OSW_RAII_FS_API_H_
