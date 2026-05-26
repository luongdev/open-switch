# tests/unit/events/CMakeLists.txt
#
# W2 Event-plane tests. The split mirrors src/events/:
#
#   tier_test          — links osw_events only (pure-C++ classifier).
#   ring_test          — links osw_events only (pure-C++ MPSC ring).
#   envelope_test      — links osw_events_test_helpers (mock seam).
#   broadcaster_test   — links osw_events only (broadcaster source is split
#                        so the threading logic doesn't need a real
#                        switch_event_t — it operates on the
#                        already-serialised shared_ptr<const string>).
#   subscribe_replay_test — links osw_events only (replay logic operates on
#                            the ring's SinceSeq query; no FS dependency).
#   binder_test        — links osw_events_test_helpers (mock seam — covers
#                        switch_event_bind invocation + exception boundary).

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

osw_add_unit_test(
    NAME    envelope_test
    LABEL   "events;unit"
    SOURCES envelope_test.cc
    LIBS    osw_events_test_helpers osw_events osw_proto
)

osw_add_unit_test(
    NAME    broadcaster_test
    LABEL   "events;unit"
    SOURCES broadcaster_test.cc
    LIBS    osw_events
)

osw_add_unit_test(
    NAME    subscribe_replay_test
    LABEL   "events;unit"
    SOURCES subscribe_replay_test.cc
    LIBS    osw_events
)

osw_add_unit_test(
    NAME    binder_test
    LABEL   "events;unit"
    SOURCES binder_test.cc
    LIBS    osw_events_test_helpers osw_events
)
