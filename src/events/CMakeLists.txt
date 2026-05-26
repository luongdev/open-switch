# src/events/CMakeLists.txt
#
# W2 Event-plane subsystem.
#
# Two libraries (mirrors the W1 observability split):
#
#   osw_events           — FS-agnostic core: tier classifier + envelope
#                          builder (envelope-builder body talks to FS via
#                          the fs_api.h shim, so it routes through the
#                          mock under OSW_TEST_FS_MOCK=1 in tests, and
#                          through real <switch.h> in the production
#                          osw_events_fs lib). Plus ring + subscriber
#                          send queue (pure C++).
#
#   osw_events_fs        — FS-dependent half: Binder (switch_event_bind
#                          registration), broadcaster (one thread per
#                          tier; pulls from rings; pushes to subscribers).
#                          Includes <switch.h> via fs_api.h.
#
# Tests of envelope / binder use osw_events_test_helpers which compiles
# the same .cc TUs with OSW_TEST_FS_MOCK=1.

# --- osw_events (FS-agnostic) ----------------------------------------
add_library(osw_events STATIC
    tier.cc
    ring.cc
    envelope.cc
    subscribe/send_queue.cc
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

# --- osw_events_fs (FS-dependent: Binder + Broadcaster + Subscribe RPC) ---
#
# These TUs include <switch.h> (via fs_api.h without the mock define) and
# link into the production module .so only. Unit tests use
# osw_events_test_helpers to exercise envelope.cc + binder.cc against
# the mock.
add_library(osw_events_fs STATIC
    binder.cc
    subscribe/broadcaster.cc
    subscribe/handler.cc
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
#
# Compiles envelope.cc + binder.cc against the FS-mock seam so unit tests
# can drive the FS-touching code paths without linking <switch.h>. NEVER
# linked into the production module .so.
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
