/*
 * src/security/eavesdrop_detector.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_detector.h"

#include <cstring>
#include <exception>

#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"
#include "osw/raii/session_lock.h"
#include "osw/security/eavesdrop_audit.h"
#include "osw/security/eavesdrop_marker.h"
#include "osw/security/eavesdrop_policy.h"

namespace osw::security {

namespace {

constexpr const char* kSubsystem = "security.eavesdrop_detector";
constexpr const char* kBindId = "mod_open_switch_eavesdrop_detector";

bool IsTrue(const char* value) noexcept {
    return value && std::strcmp(value, "true") == 0;
}

void OnMediaBugStart(switch_event_t* event) noexcept {
    try {
        if (!event) {
            return;
        }
        const char* bug_fn = ::osw::raii::fs::EventGetHeader(event, "Media-Bug-Function");
        if (!bug_fn || std::strcmp(bug_fn, "eavesdrop") != 0) {
            return;
        }
        const char* uuid = ::osw::raii::fs::EventGetHeader(event, "Unique-ID");
        if (!uuid || *uuid == '\0') {
            return;
        }

        osw::SessionLock target(uuid);
        if (!target) {
            return;
        }
        switch_channel_t* target_channel = target.channel();
        if (!target_channel) {
            return;
        }

        const char* marked = ::osw::raii::fs::ChannelGetVariable(target_channel, kBotSessionVar);
        if (!IsTrue(marked)) {
            return;
        }

        const char* policy_raw =
            ::osw::raii::fs::ChannelGetVariable(target_channel, kEavesdropPolicyVar);
        const EavesdropPolicy policy =
            policy_raw ? ParseEavesdropPolicy(policy_raw) : EavesdropPolicy::kDeny;
        const bool fail_closed = policy == EavesdropPolicy::kDeny;
        EmitEavesdropAudit(nullptr,
                           target.get(),
                           policy,
                           "2_post_attach_detection",
                           fail_closed ? "detected_hangup_target" : "detected_only");
        if (fail_closed) {
            (void)::osw::raii::fs::ChannelHangup(target_channel, SWITCH_CAUSE_CALL_REJECTED);
            osw::log::Warn(kSubsystem,
                           "raw eavesdrop detected on deny-policy bot session; "
                           "fail-closed target hangup uuid=%s",
                           uuid);
        }
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "MEDIA_BUG_START detector exception: %s", e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "MEDIA_BUG_START detector unknown exception");
    }
}

}  // namespace

bool BindEavesdropDetector() noexcept {
    const switch_status_t status = ::osw::raii::fs::EventBind(kBindId,
                                                              SWITCH_EVENT_MEDIA_BUG_START,
                                                              /*subclass_name=*/nullptr,
                                                              &OnMediaBugStart,
                                                              /*user_data=*/nullptr);
    if (status != SWITCH_STATUS_SUCCESS) {
        osw::log::Error(kSubsystem,
                        "switch_event_bind MEDIA_BUG_START failed: status=%d",
                        static_cast<int>(status));
        return false;
    }
    return true;
}

void UnbindEavesdropDetector() noexcept {
    const switch_status_t status = ::osw::raii::fs::EventUnbindCallback(&OnMediaBugStart);
    if (status != SWITCH_STATUS_SUCCESS) {
        osw::log::Debug(kSubsystem,
                        "switch_event_unbind_callback MEDIA_BUG_START returned %d",
                        static_cast<int>(status));
    }
}

void HandleMediaBugStartForTest(switch_event_t* event) noexcept {
    OnMediaBugStart(event);
}

}  // namespace osw::security
