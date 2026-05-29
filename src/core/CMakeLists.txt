# src/core/CMakeLists.txt — module lifecycle, config, singleton.
#
# Two libs:
#   osw_core      — FS-agnostic (validation, lifecycle, ...).
#                   Linked by tests + the module .so.
#   osw_core_fs   — FS-dependent (config XML loader via FF-013;
#                   Module::Load that touches FS pool + interface).
#                   Linked only by the module .so.

add_library(osw_core STATIC
    config.cc
    lifecycle.cc
)
target_include_directories(osw_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_core PUBLIC cxx_std_20)
target_link_libraries(osw_core PUBLIC
    osw_observability
    osw_security
    Threads::Threads
)
set_target_properties(osw_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(osw_core_fs STATIC
    config_fs.cc
    module.cc
)
target_include_directories(osw_core_fs PRIVATE
    ${OSW_FREESWITCH_INCLUDE_DIR}
)
target_include_directories(osw_core_fs PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_core_fs PUBLIC cxx_std_20)
target_compile_definitions(osw_core_fs PRIVATE SWITCH_API_VISIBILITY=1)
target_link_libraries(osw_core_fs PUBLIC
    osw_core
    osw_control          # Module::Load owns the GrpcServer
    osw_events           # RingSet + Broadcaster + TierClassifier
    osw_events_fs        # Binder (switch_event_bind shim)
    osw_observability
    osw_observability_fs
    osw_security_fs
)
set_target_properties(osw_core_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)
