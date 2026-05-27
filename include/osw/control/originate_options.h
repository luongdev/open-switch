/*
 * include/osw/control/originate_options.h
 *
 * osw::control::OriginateOptions — builder that materialises an
 * OriginateRequest into the parameter shape switch_ivr_originate
 * expects.
 *
 * switch_ivr_originate signature (verified at v1.10.12 — FF-021):
 *
 *   switch_status_t switch_ivr_originate(
 *       switch_core_session_t *session,   // NULL for unattended
 *       switch_core_session_t **bleg,     // output: new session
 *       switch_call_cause_t   *cause,     // output: result cause
 *       const char            *bridgeto,  // dial string
 *       uint32_t               timelimit_sec,
 *       const switch_state_handler_table_t *table,  // NULL OK
 *       const char            *cid_name_override,
 *       const char            *cid_num_override,
 *       switch_caller_profile_t *caller_profile_override, // NULL OK
 *       switch_event_t        *ovars,     // channel variables
 *       switch_originate_flag_t flags,
 *       switch_call_cause_t   *cancel_cause, // NULL OK
 *       switch_dial_handle_t  *dh);       // NULL OK
 *
 * V1 always uses the unattended originate path (session == NULL).
 * The caller constructs OriginateOptions, checks Valid(), then calls
 * switch_ivr_originate through the RAII wrapper with the parameters
 * exposed by this type.
 *
 * Memory ownership:
 *   - `dial_string()`, `cid_name()`, `cid_num()` return references to
 *     strings owned by this object for the object's lifetime.
 *   - `ovars()` returns the switch_event_t* whose lifetime is also
 *     owned by this object. The caller MUST NOT destroy it — ownership
 *     is transferred to switch_ivr_originate on the successful call
 *     (FS sets the *ovars pointer to NULL after consuming it via
 *     switch_event_destroy internally). Call ReleaseOvars() after the
 *     originate call if you need to observe that the pointer was
 *     consumed; dtor calls EventDestroy on a non-null ovars_.
 *
 * Cited FACTs:
 *   - FF-021 — switch_ivr_originate signature + ownership.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_ORIGINATE_OPTIONS_H_
#define OSW_CONTROL_ORIGINATE_OPTIONS_H_

#include <cstdint>
#include <string>

#include "open_switch/control/v1/control.pb.h"

#include "osw/raii/fs_api.h"

namespace osw::control {

/// Builder for switch_ivr_originate parameters.
///
/// Not copyable (owns a switch_event_t*).
class OriginateOptions {
  public:
    /// Build from a validated OriginateRequest.
    ///
    /// `req` must outlive the Build call but not the object. Returns
    /// an options object; check Valid() before using. An invalid object
    /// is returned when:
    ///   - All endpoints are empty.
    ///   - Timeout is zero or negative.
    ///   - switch_event_create failed (ovars alloc failure).
    [[nodiscard]] static OriginateOptions Build(
        const open_switch::control::v1::OriginateRequest& req) noexcept;

    ~OriginateOptions() noexcept;

    OriginateOptions(const OriginateOptions&) = delete;
    OriginateOptions& operator=(const OriginateOptions&) = delete;

    OriginateOptions(OriginateOptions&& other) noexcept;
    OriginateOptions& operator=(OriginateOptions&& other) noexcept;

    /// True iff the object was built successfully.
    [[nodiscard]] bool Valid() const noexcept { return valid_; }

    /// Error message when !Valid().
    [[nodiscard]] const std::string& ErrorMessage() const noexcept { return error_; }

    /// The constructed dial string (e.g. "sofia/gateway/gw1/+441234567890").
    [[nodiscard]] const std::string& dial_string() const noexcept { return dial_string_; }

    /// Caller ID name (empty string = no override).
    [[nodiscard]] const std::string& cid_name() const noexcept { return cid_name_; }

    /// Caller ID number (empty string = no override).
    [[nodiscard]] const std::string& cid_num() const noexcept { return cid_num_; }

    /// Originate timeout in seconds (always > 0 when Valid()).
    [[nodiscard]] uint32_t timeout_sec() const noexcept { return timeout_sec_; }

    /// The channel-variable event passed as `ovars` to
    /// switch_ivr_originate. Null if no variables were set.
    /// Ownership: still held by this object until ReleaseOvars() is
    /// called (or the dtor runs).
    [[nodiscard]] switch_event_t* ovars() const noexcept { return ovars_; }

    /// Transfer ownership of ovars_ to the caller. After this call
    /// ovars() returns nullptr and the dtor will not destroy the event.
    /// Used to pass ownership to switch_ivr_originate (which consumes
    /// it via switch_event_destroy internally on FF-021).
    switch_event_t* ReleaseOvars() noexcept {
        switch_event_t* ev = ovars_;
        ovars_ = nullptr;
        return ev;
    }

  private:
    OriginateOptions() noexcept = default;

    bool valid_ = false;
    std::string error_;
    std::string dial_string_;
    std::string cid_name_;
    std::string cid_num_;
    uint32_t timeout_sec_ = 0;
    switch_event_t* ovars_ = nullptr;
};

}  // namespace osw::control

#endif  // OSW_CONTROL_ORIGINATE_OPTIONS_H_
