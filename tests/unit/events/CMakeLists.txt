# tests/unit/events/CMakeLists.txt
#
# W2 Event-plane tests. Each subsequent W2 commit appends an
# osw_add_unit_test() entry as the source files land.

osw_add_unit_test(
    NAME    tier_test
    LABEL   "events;unit"
    SOURCES tier_test.cc
    LIBS    osw_events
)

osw_add_unit_test(
    NAME    ring_test
    LABEL   "events;unit"
    SOURCES ring_test.cc
    LIBS    osw_events
)
