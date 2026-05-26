/*
 * src/observability/audit.cc
 *
 * Implementation of osw::audit::Emit. See include/osw/observability/audit.h
 * for the public contract.
 *
 * Build-mode contract (identical to the W1 RAII helpers):
 *
 *   - Production build: compiled with the project default (no
 *     OSW_TEST_FS_MOCK). fs_api.h pulls in <switch.h> and the FS calls
 *     route to real switch_event_* functions.
 *
 *   - Test build: compiled into the dedicated `osw_audit_test_helpers`
 *     STATIC lib with `-DOSW_TEST_FS_MOCK=1`. fs_api.h then includes
 *     fs_mock.h and every FS call goes through the captured-state mock.
 *
 * The TU itself is unaware of which mode it is in — both share the
 * same headers and the same logic.
 *
 * FACTs cited:
 *   - FF-017: switch_event_{create,destroy,fire} ownership.
 *   - FF-020: switch_event_create_subclass + Event-Subclass header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/audit.h"

#include <exception>
#include <string>
#include <vector>

#include "osw/observability/log.h"
#include "osw/raii/event_guard.h"
#include "osw/raii/fs_api.h"

namespace osw::audit {

namespace {

constexpr const char* kSubsystem = "audit";

// Internal core: returns false on any failure path. Never throws.
bool EmitImpl(std::string_view name, const std::vector<Header>& headers) noexcept {
    if (name.empty()) {
        osw::log::Warn(kSubsystem, "audit::Emit refused empty name");
        return false;
    }

    // Build the full subclass: "osw.audit." + name.
    std::string full_subclass;
    full_subclass.reserve(kSubclassPrefix.size() + name.size());
    full_subclass.append(kSubclassPrefix);
    full_subclass.append(name);

    // FF-020: switch_event_create_subclass for SWITCH_EVENT_CUSTOM with a
    // non-NULL subclass. Returns SUCCESS with *ev populated, or any other
    // status with *ev = NULL. We adopt into an EventGuard so the
    // destructor path destroys the event if we never fire it (e.g.,
    // header-add failure).
    switch_event_t* raw = nullptr;
    switch_status_t s =
        ::osw::raii::fs::EventCreateSubclass(&raw, SWITCH_EVENT_CUSTOM, full_subclass.c_str());
    if (s != SWITCH_STATUS_SUCCESS || raw == nullptr) {
        // log::Warn so the failure is visible without spamming at INFO.
        osw::log::Warn(kSubsystem,
                       "audit::Emit: switch_event_create_subclass failed (status=%d) "
                       "for subclass='%s'",
                       static_cast<int>(s),
                       full_subclass.c_str());
        return false;
    }
    osw::EventGuard guard = osw::EventGuard::adopt(raw);

    // Add caller-supplied headers via switch_event_add_header_string.
    // The FS call DUPs both name and value into the event's allocation,
    // so we don't need to keep the source buffers alive past the call.
    for (const auto& h : headers) {
        switch_status_t hs = ::osw::raii::fs::EventAddHeaderString(
            guard.get(), SWITCH_STACK_BOTTOM, h.name.c_str(), h.value.c_str());
        if (hs != SWITCH_STATUS_SUCCESS) {
            // Log and continue — a single header failure should not abort
            // the audit emission. The event still has Event-Subclass + any
            // headers that DID succeed.
            osw::log::Warn(kSubsystem,
                           "audit::Emit: add_header_string failed (status=%d) "
                           "for subclass='%s' header='%s'",
                           static_cast<int>(hs),
                           full_subclass.c_str(),
                           h.name.c_str());
        }
    }

    // FF-017: fire() transfers ownership to FS (on success the underlying
    // event is set to NULL by FS; EventGuard mirrors that on its side).
    // On failure FS still destroys the event internally. Either way the
    // guard is empty after the call, so the destructor is a no-op.
    switch_status_t fs = guard.fire();
    if (fs != SWITCH_STATUS_SUCCESS) {
        osw::log::Warn(kSubsystem,
                       "audit::Emit: switch_event_fire failed (status=%d) "
                       "for subclass='%s'",
                       static_cast<int>(fs),
                       full_subclass.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool Emit(std::string_view name, const std::vector<Header>& headers) noexcept {
    try {
        return EmitImpl(name, headers);
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "audit::Emit threw: %s", e.what());
        return false;
    } catch (...) {
        osw::log::Error(kSubsystem, "audit::Emit threw unknown exception");
        return false;
    }
}

bool Emit(std::string_view name, HeadersInit headers) noexcept {
    std::vector<Header> vec(headers.begin(), headers.end());
    return Emit(name, vec);
}

}  // namespace osw::audit
