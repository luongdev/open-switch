/*
 * include/osw/security/eavesdrop_app.h
 *
 * W7 Track A osw_eavesdrop dialplan app registration.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_SECURITY_EAVESDROP_APP_H_
#define OSW_SECURITY_EAVESDROP_APP_H_

#include "osw/raii/fs_api.h"

namespace osw::security {

void RegisterOswEavesdropApp(switch_loadable_module_interface_t** module_interface) noexcept;

/// Exposed for FS-mock unit tests; production invokes the same body via
/// the registered SWITCH_STANDARD_APP trampoline.
void InvokeOswEavesdropForTest(switch_core_session_t* session, const char* data) noexcept;

}  // namespace osw::security

#endif  // OSW_SECURITY_EAVESDROP_APP_H_
