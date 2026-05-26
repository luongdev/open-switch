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
# Each W2 commit MUST leave the project buildable, so the source lists
# below reflect ONLY what exists on disk in the current commit. The
# follow-up commits in the W2 sequence (ring, envelope, binder,
# broadcaster, handler) will append to these lists as they land.

# --- osw_events (FS-agnostic) ----------------------------------------
#
# Sources grow commit-by-commit:
#   tier.cc            (current commit)
#   ring.cc            (added by ring commit)
#   envelope.cc        (added by envelope commit; ALSO compiled into
#                       osw_events_test_helpers under OSW_TEST_FS_MOCK=1)
#   subscribe/send_queue.cc (added by broadcaster commit)
add_library(osw_events STATIC
    tier.cc
    ring.cc
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
