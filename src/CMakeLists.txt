# src/CMakeLists.txt — mod_open_switch module shared lib.
#
# W1 Foundation: subsystem static libs link into a single
# mod_open_switch.so module. Each subsystem is an internal STATIC lib;
# they are NEVER installed standalone. The module exports only the
# `mod_open_switch_module_interface` data symbol via
# SWITCH_MOD_DECLARE_DATA (FREESWITCH-FACTS FF-009) — everything else
# is hidden by the root CMakeLists.txt's -fvisibility=hidden plus
# -Wl,--exclude-libs,ALL (FF-010 + N-5 round-3 work).
#
# Layout:
#   src/observability/  osw_observability + osw_observability_fs
#   src/core/           osw_core   (config, module singleton, lifecycle)
#   src/control/        osw_control (gRPC server, handlers)
#   src/mod_open_switch.cc — the SWITCH_MODULE_DEFINITION + thin
#                            wrappers around osw::Module
#
# Tests link the FS-agnostic libs only (osw_observability, osw_core,
# osw_control_skeleton). The FS-dependent libs (osw_observability_fs,
# osw_core_fs, osw_control_fs) are linked exclusively into the .so.

add_subdirectory(observability)
add_subdirectory(security)
add_subdirectory(control)
add_subdirectory(core)
add_subdirectory(events)
add_subdirectory(media)

# --- The module .so ---------------------------------------------------

add_library(mod_open_switch SHARED
    mod_open_switch.cc
)

target_include_directories(mod_open_switch
    PRIVATE
        ${OSW_FREESWITCH_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/include
)

# SWITCH_API_VISIBILITY gates the SWITCH_MOD_DECLARE_DATA macro
# (src/include/switch_platform.h:186-193 of FreeSWITCH v1.10.12).
# With it defined, SWITCH_MOD_DECLARE_DATA expands to
# __attribute__((visibility("default"))), which is required to export
# `mod_open_switch_module_interface` as a global data symbol that
# FreeSWITCH's dlopen+dlsym loader can find.  Without it, our global
# -fvisibility=hidden would hide the interface symbol and
# `load mod_open_switch` would fail with "Cannot locate symbol" or
# similar.  See FREESWITCH-FACTS FF-009.
target_compile_definitions(mod_open_switch
    PRIVATE
        SWITCH_API_VISIBILITY=1
)

# The output must be named `mod_open_switch.so` exactly (no `lib`
# prefix) so that FreeSWITCH's loader finds it under MOD_DIR.
set_target_properties(mod_open_switch PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mod_open_switch"
    SUFFIX ".so"
)

# Link the subsystem static libs PRIVATE — none of their symbols leak
# out of mod_open_switch.so. Combined with -fvisibility=hidden and
# -Wl,--exclude-libs,ALL (root CMakeLists.txt), this isolates the
# module's namespace (FREESWITCH-FACTS FF-010 + Codex round-2 N-5).
#
# Static-lib dead-code elimination caveat
# ---------------------------------------
# `osw_control_fs` carries the W3 ControlService method overrides
# (Originate, Hangup, HangupMany, Bridge, Execute, BlindTransfer,
# SetVariables, Hold, Unhold). They are referenced ONLY via the
# ControlServiceSkeleton vtable, which is emitted in
# `osw_control/handlers/health_handler.cc.o`. GNU ld is single-pass:
# when it scans osw_control_fs before osw_control, no undefined
# vtable references exist yet, so it skips the W3 handler .o files.
# When osw_control's vtable later introduces those references, ld
# is past osw_control_fs and the symbols stay undefined in the
# resulting .so. FS dlopen then fails with
#   "undefined symbol: _ZN3osw7control22ControlServiceSkeleton9Originate..."
# (caught by the runtime smoke test in examples/runtime).
#
# Fix: wrap osw_control_fs in --whole-archive so all its .o files
# are pulled into the .so unconditionally. The -Wl,--exclude-libs,ALL
# at the top level still hides the symbols externally.
#
# osw_observability_fs has the same indirect-reference pattern via
# the audit-emit hook; we apply --whole-archive there too as a
# belt-and-braces measure (cheap; the lib is small).
target_link_libraries(mod_open_switch
    PRIVATE
        osw_core_fs
        osw_core
        osw_events_fs
        osw_events
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,osw_control_fs>"
        osw_control
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,osw_observability_fs>"
        osw_observability
        osw_security_fs
        osw_security
        osw_media_fs
        osw_media
        osw_proto
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::UUID
)

if(OSW_INSTALL_MODULE)
    install(TARGETS mod_open_switch
        LIBRARY DESTINATION ${OSW_FREESWITCH_MOD_DIR}
    )
endif()
