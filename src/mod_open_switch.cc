/*
 * mod_open_switch — Phase 1 stub
 *
 * Copyright (C) 2026 luongdev
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the Phase-1 stub. It exists so that:
 *
 *   1. The CMake + Dockerfile.builder pipeline produces a real
 *      mod_open_switch.so end-to-end. Until this stub landed the
 *      builder image's `cp /usr/local/mod/mod_open_switch.so` step
 *      failed because the target was never produced (see Codex
 *      round-2 finding N6).
 *
 *   2. A FreeSWITCH operator can `load mod_open_switch` without
 *      crashing. The module reports a single log line on load and
 *      does nothing else. Control gRPC server, event bind, media
 *      bug manager — all implementation phase work.
 *
 *   3. CI's build-and-asan job has a real .so to ldd-verify and to
 *      sanitize. The MIN_INTEGRATION_TESTS gate stays at `if: false`
 *      until the first real implementation commit replaces this
 *      stub's load function with the actual subsystems.
 *
 * The macros and signatures come from FREESWITCH-FACTS FF-009
 * (src/include/switch_types.h:2600-2647 +
 * src/include/switch_platform.h:184-200 on the GCC/Linux branch).
 *
 * Compilation requirements (CMakeLists.txt enforces these):
 *   -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
 *   -Wl,--exclude-libs,ALL
 * ASAN + LSAN: trivially clean — no allocations.
 */

#include <switch.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown);

SWITCH_MODULE_DEFINITION(mod_open_switch, mod_open_switch_load,
                         mod_open_switch_shutdown, NULL);

SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load) {
    // The `pool` argument is the module's APR pool; the stub does
    // not own any resources, so we ignore it. The
    // `module_interface` out-parameter must be populated even for a
    // stub module — FreeSWITCH dereferences it after we return.
    *module_interface = switch_loadable_module_create_module_interface(
        pool, modname);

    if (*module_interface == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_open_switch: failed to allocate "
                          "module interface\n");
        return SWITCH_STATUS_GENERR;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_open_switch loaded (stub)\n");
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_open_switch shutdown (stub)\n");
    return SWITCH_STATUS_SUCCESS;
}
