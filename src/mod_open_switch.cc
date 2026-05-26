/*
 * mod_open_switch — W1 Foundation
 *
 * Copyright (C) 2026 luongdev
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Thin C-callable shell. SWITCH_MODULE_DEFINITION emits the
 * mod_open_switch_module_interface symbol (FF-009;
 * src/include/switch_types.h:2607-2647 in v1.10.12) which the FS
 * loader finds via dlsym. The load + shutdown functions delegate
 * everything to osw::Module — the C++ singleton in src/core/module.cc
 * — wrapped in a try/catch boundary per
 * designs/memory-management.md §"Exception-safety boundary".
 *
 * Compilation requirements (src/CMakeLists.txt enforces these):
 *   -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
 *   -Wl,--exclude-libs,ALL
 *   -DSWITCH_API_VISIBILITY=1
 *
 * SWITCH_API_VISIBILITY=1 is required for SWITCH_MOD_DECLARE_DATA to
 * expand to __attribute__((visibility("default"))) so the module
 * interface symbol is reachable by the loader despite our global
 * -fvisibility=hidden. See FF-009 §"Excerpt — switch_platform.h:184-204".
 *
 * Memory safety:
 *   - No allocations directly in load / shutdown — all heap activity
 *     lives inside osw::Module, behind smart-pointers / RAII.
 *   - Exception boundary: every C-callable function below wraps its
 *     body in try { ... } catch (std::exception&) { ... } catch (...).
 *     Returns SWITCH_STATUS_GENERR to FS on caught exceptions; never
 *     re-throws.
 *   - No raw FS API call here beyond the singleton dispatcher.
 *     Module::Load is the entry point for everything Foundation-wave
 *     does with the FS pool + interface (FF-014).
 */

#include <exception>

#include <switch.h>

#include "osw/core/module.h"
#include "osw/observability/log.h"

// FF-009: declare-then-define the load + shutdown entry points so
// SWITCH_MODULE_DEFINITION can take their addresses.
SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown);

// FF-009: emits the global data symbol mod_open_switch_module_interface
// of type switch_loadable_module_function_table_t. SMODF_NONE keeps
// the default RTLD_LOCAL load semantics (FF-010) — we do NOT want our
// statically-linked gRPC + protobuf + abseil symbols to leak into other
// FS modules.
SWITCH_MODULE_DEFINITION(mod_open_switch,
                         mod_open_switch_load,
                         mod_open_switch_shutdown,
                         NULL);

SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load) {
    // The `pool` argument is the module's APR pool (FF-014 — pool
    // owns module-scoped allocations). The `module_interface`
    // out-parameter must be populated; FreeSWITCH dereferences it
    // after we return.
    try {
        // FF-014: switch_loadable_module_create_module_interface
        // allocates from `pool` via switch_core_alloc; we do NOT free
        // the returned ptr. We store it as a non-owning view inside
        // osw::Module for later subsystem registration.
        *module_interface = switch_loadable_module_create_module_interface(
            pool, modname);

        if (*module_interface == NULL) {
            // FF-012: switch_log_printf is safe to call from any
            // thread and from module load specifically.
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "mod_open_switch: failed to allocate "
                              "module interface\n");
            return SWITCH_STATUS_GENERR;
        }

        if (!osw::Module::Instance().Load(pool, *module_interface)) {
            // osw::Module::Load logs the detailed failure cause.
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "mod_open_switch: Module::Load returned false\n");
            return SWITCH_STATUS_GENERR;
        }
        return SWITCH_STATUS_SUCCESS;
    } catch (const std::exception& e) {
        // FF-012: switch_log_printf with "%s" — `e.what()` is user-data,
        // never the format string.
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_open_switch_load: exception: %s\n", e.what());
        return SWITCH_STATUS_GENERR;
    } catch (...) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_open_switch_load: unknown exception\n");
        return SWITCH_STATUS_GENERR;
    }
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown) {
    try {
        const bool ok = osw::Module::Instance().Shutdown();
        return ok ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_GENERR;
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_open_switch_shutdown: exception: %s\n", e.what());
        return SWITCH_STATUS_GENERR;
    } catch (...) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_open_switch_shutdown: unknown exception\n");
        return SWITCH_STATUS_GENERR;
    }
}
