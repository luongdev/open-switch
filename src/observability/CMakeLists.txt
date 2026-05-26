# src/observability/CMakeLists.txt
#
# Two libraries:
#
#   osw_observability      — the public API (log.cc + health.cc). Has
#                            no <switch.h> dependency. Linked by both
#                            the module .so and the test binaries.
#
#   osw_observability_fs   — the FS-dependent default log sink. Linked
#                            into the module .so only. Tests install
#                            their own sink and never reach this code.

# --- osw_observability (FS-agnostic) ---------------------------------
add_library(osw_observability STATIC
    log.cc
    health.cc
)
target_include_directories(osw_observability PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_observability PUBLIC cxx_std_20)
target_link_libraries(osw_observability PUBLIC
    Threads::Threads
)
set_target_properties(osw_observability PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_observability_fs (FS-dependent default sink + audit) --------
#
# These TUs include <switch.h> (audit.cc transitively via fs_api.h). We
# declare them as a separate static lib so the unit tests can link
# osw_observability WITHOUT pulling in <switch.h>. Tests of audit.cc
# compile the same source with -DOSW_TEST_FS_MOCK=1 via the dedicated
# osw_audit_test_helpers target below.
add_library(osw_observability_fs STATIC
    log_default_sink.cc
    audit.cc
)
target_include_directories(osw_observability_fs PRIVATE
    ${OSW_FREESWITCH_INCLUDE_DIR}
)
target_include_directories(osw_observability_fs PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_observability_fs PUBLIC cxx_std_20)
target_link_libraries(osw_observability_fs PUBLIC
    osw_observability
)
# SWITCH_API_VISIBILITY=1 keeps the SWITCH_MOD_DECLARE_DATA macro
# alive for any FS macros that expand inside the TU. This static
# lib never EXPORTS the visibility-default symbol itself (only the
# top-level mod_open_switch.cc does), but the macro must still be in
# scope so the FS headers compile correctly under our -fvisibility=hidden
# discipline (FREESWITCH-FACTS FF-009).
target_compile_definitions(osw_observability_fs PRIVATE SWITCH_API_VISIBILITY=1)
set_target_properties(osw_observability_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_audit_test_helpers (test-only, FS-mock seam) ----------------
#
# Compiles audit.cc against the FS-mock seam (OSW_TEST_FS_MOCK=1) so the
# unit tests can drive Emit() and assert on captured switch_event state.
# This library is built ONLY when OSW_BUILD_TESTS=ON and is NEVER linked
# into the production mod_open_switch.so.
if(OSW_BUILD_TESTS)
    add_library(osw_audit_test_helpers STATIC
        audit.cc
    )
    target_include_directories(osw_audit_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_features(osw_audit_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_audit_test_helpers PUBLIC
        osw_observability
    )
    target_compile_definitions(osw_audit_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    # Tests typically run under -Wno-error; suppress the project-wide
    # -Werror for this helper TU so a future warning in audit.cc doesn't
    # break the test build before it can be fixed.
    target_compile_options(osw_audit_test_helpers PRIVATE
        -Wno-error
    )
endif()
