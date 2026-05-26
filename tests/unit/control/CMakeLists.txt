# tests/unit/control/CMakeLists.txt — gRPC server smoke tests.
#
# These tests need a real gRPC runtime. CI runs them in the builder
# container; on dev hosts without gRPC the test fails to link, which
# matches the contract's "if gRPC isn't installed locally, document
# the gap; CI runs in the builder container".
#
# libfreeswitch link is contributed via INTERFACE by osw_observability_fs
# (audit.cc references switch_event_* symbols). See
# src/observability/CMakeLists.txt — declaring the dep there is what
# makes CMake place -lfreeswitch AFTER libosw_observability_fs.a in the
# link line, satisfying GNU ld single-pass.

osw_add_unit_test(
    NAME    server_test
    LABEL   "control;unit"
    SOURCES server_test.cc
    # osw_control comes FIRST so the linker sees the unresolved
    # SubscribeEvents reference (from ControlServiceSkeleton's vtable
    # in health_handler.cc.o) before scanning osw_control_fs, which
    # carries the real implementation.
    LIBS    osw_control osw_control_fs
)
