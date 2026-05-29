/*
 * src/security/eavesdrop_app.cc
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/security/eavesdrop_app.h"

#include <cstring>
#include <exception>

#if !defined(OSW_TEST_FS_MOCK)
#include <switch.h>
#endif

#include "osw/observability/log.h"
#include "osw/raii/fs_api.h"
#include "osw/raii/session_lock.h"
#include "osw/security/eavesdrop_audit.h"
#include "osw/security/eavesdrop_marker.h"
#include "osw/security/eavesdrop_policy.h"

namespace osw::security {

namespace {

constexpr const char* kSubsystem = "security.eavesdrop_app";

bool IsTrue(const char* value) noexcept {
    return value && std::strcmp(value, "true") == 0;
}

void HandleOswEavesdropApp(switch_core_session_t* session, const char* data) noexcept {
    try {
        if (!session) {
            return;
        }
        if (!data || *data == '\0') {
            osw::log::Warn(kSubsystem, "osw_eavesdrop: missing target UUID");
            return;
        }

        bool delegate_to_fs = false;
        {
            osw::SessionLock target(data);
            if (!target) {
                osw::log::Warn(kSubsystem, "osw_eavesdrop: target %s not found", data);
                return;
            }

            switch_channel_t* target_channel = target.channel();
            const char* marked =
                target_channel ? ::osw::raii::fs::ChannelGetVariable(target_channel, kBotSessionVar)
                               : nullptr;
            if (!IsTrue(marked)) {
                delegate_to_fs = true;
            } else {
                const char* policy_raw =
                    ::osw::raii::fs::ChannelGetVariable(target_channel, kEavesdropPolicyVar);
                const EavesdropPolicy policy =
                    policy_raw ? ParseEavesdropPolicy(policy_raw) : EavesdropPolicy::kDeny;
                const bool denied = (policy == EavesdropPolicy::kDeny);
                EmitEavesdropAudit(
                    session, target.get(), policy, "1_pre_attach", denied ? "hangup" : "permitted");
                if (denied) {
                    switch_channel_t* supervisor_channel =
                        ::osw::raii::fs::SessionGetChannel(session);
                    (void)::osw::raii::fs::ChannelHangup(supervisor_channel,
                                                         SWITCH_CAUSE_CALL_REJECTED);
                    return;
                }
                delegate_to_fs = true;
            }
        }

        if (delegate_to_fs) {
            (void)::osw::raii::fs::IvrEavesdropSession(session, data);
        }
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "osw_eavesdrop: exception: %s", e.what());
    } catch (...) {
        osw::log::Error(kSubsystem, "osw_eavesdrop: unknown exception");
    }
}

#if !defined(OSW_TEST_FS_MOCK)
SWITCH_STANDARD_APP(OswEavesdropAppFunction) {
    HandleOswEavesdropApp(session, data);
}
#endif

}  // namespace

void RegisterOswEavesdropApp(switch_loadable_module_interface_t** module_interface) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    (void)module_interface;
#else
    if (!module_interface || !*module_interface) {
        return;
    }
    switch_application_interface_t* app_interface = nullptr;
    SWITCH_ADD_APP(app_interface,
                   "osw_eavesdrop",
                   "Policy-aware eavesdrop for bot calls",
                   "Wraps FreeSWITCH eavesdrop with mod_open_switch bot-call policy checks",
                   OswEavesdropAppFunction,
                   "<target-uuid>",
                   SAF_NONE);
#endif
}

void InvokeOswEavesdropForTest(switch_core_session_t* session, const char* data) noexcept {
    HandleOswEavesdropApp(session, data);
}

}  // namespace osw::security
