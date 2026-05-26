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
add_subdirectory(control)
add_subdirectory(core)

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
# Order matters for static-lib link: list dependents first.
target_link_libraries(mod_open_switch
    PRIVATE
        osw_core_fs
        osw_core
        osw_control
        osw_observability_fs
        osw_observability
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
