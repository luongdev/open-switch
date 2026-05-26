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

# --- osw_observability_fs (FS-dependent default sink) ----------------
#
# This TU includes <switch.h>. We declare it as a separate static lib
# so the unit tests can link osw_observability WITHOUT pulling in
# log_default_sink.cc (which would force the test build to know about
# <switch.h>).
add_library(osw_observability_fs STATIC
    log_default_sink.cc
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
