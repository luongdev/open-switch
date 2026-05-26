# tests/unit/control/CMakeLists.txt — gRPC server smoke tests.
#
# These tests need a real gRPC runtime. CI runs them in the builder
# container; on dev hosts without gRPC the test fails to link, which
# matches the contract's "if gRPC isn't installed locally, document
# the gap; CI runs in the builder container".

osw_add_unit_test(
    NAME    server_test
    LABEL   "control;unit"
    SOURCES server_test.cc
    # osw_control comes FIRST so the linker sees the unresolved
    # SubscribeEvents reference (from ControlServiceSkeleton's vtable
    # in health_handler.cc.o) before scanning osw_control_fs, which
    # carries the real implementation. Reversing this order causes
    # the linker to skip subscribe_events_handler.cc.o because no
    # earlier file referenced it yet (static archive scan-once rule).
    # osw_control_fs transitively links osw_observability_fs, which in
    # turn provides osw::audit::Emit. The test only exercises Health
    # (not SubscribeEvents), so audit code is linked but never called.
    LIBS    osw_control osw_control_fs
)
