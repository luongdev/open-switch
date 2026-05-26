# src/events/CMakeLists.txt
#
# W2 Event-plane subsystem.
#
# Three libraries (mirrors the W1 observability split):
#
#   osw_events              — FS-agnostic core: tier classifier + ring.
#                             Pure C++; tests link this directly.
#
#   osw_events_fs           — FS-dependent: envelope builder (reads via
#                             fs_api.h shim → <switch.h>). Linked into
#                             the production module .so only. Later W2
#                             commits also add binder.cc + broadcaster
#                             + handler here.
#
#   osw_events_test_helpers — Test-only mirror of osw_events_fs that
#                             compiles the same .cc TUs against the
#                             FS-mock seam (OSW_TEST_FS_MOCK=1). NEVER
#                             linked into the production module .so.
#
# Each W2 commit MUST leave the project buildable; source lists below
# reflect ONLY files that exist on disk in the current commit.

# --- osw_events (FS-agnostic) ----------------------------------------
#
# The broadcaster + subscriber + send_queue belong here (FS-agnostic):
# they touch only Ring (in this lib), serialised envelope bytes
# (already produced by osw_events_fs), Health, and osw::log. No
# <switch.h> includes — tests link osw_events directly to exercise the
# broadcaster against test-built Rings.
add_library(osw_events STATIC
    tier.cc
    ring.cc
    ring_set.cc
    subscribe/routing.cc
    subscribe/send_queue.cc
    subscribe/subscriber.cc
    subscribe/broadcaster.cc
)
target_include_directories(osw_events PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_events PUBLIC cxx_std_20)
target_link_libraries(osw_events PUBLIC
    osw_observability
    osw_proto
    Threads::Threads
)
set_target_properties(osw_events PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_events_fs (FS-dependent) ------------------------------------
add_library(osw_events_fs STATIC
    envelope.cc
    binder.cc
)
target_include_directories(osw_events_fs PRIVATE
    ${OSW_FREESWITCH_INCLUDE_DIR}
)
target_include_directories(osw_events_fs PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_events_fs PUBLIC cxx_std_20)
target_link_libraries(osw_events_fs PUBLIC
    osw_events
    osw_observability
    osw_observability_fs
    osw_proto
    Threads::Threads
)
# SWITCH_API_VISIBILITY=1 — see comment in src/observability/CMakeLists.txt.
target_compile_definitions(osw_events_fs PRIVATE SWITCH_API_VISIBILITY=1)
set_target_properties(osw_events_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_events_test_helpers (test-only, FS-mock seam) ---------------
if(OSW_BUILD_TESTS)
    add_library(osw_events_test_helpers STATIC
        envelope.cc
        binder.cc
    )
    target_include_directories(osw_events_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_features(osw_events_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_events_test_helpers PUBLIC
        osw_events
        osw_observability
        osw_proto
    )
    target_compile_definitions(osw_events_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_events_test_helpers PRIVATE
        -Wno-error
    )
endif()
