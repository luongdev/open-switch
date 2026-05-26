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
    # carries the real implementation. osw_control_fs transitively
    # pulls osw_observability_fs (for audit::Emit), which references
    # real switch_event_* symbols — only libfreeswitch.so provides
    # them at runtime. mod_open_switch.so itself doesn't link
    # libfreeswitch (FS resolves at module load), but a standalone
    # binary must.
    LIBS    osw_control osw_control_fs
)
# Resolve the FreeSWITCH C ABI symbols that osw_observability_fs's
# audit.cc references (switch_event_create_subclass_detailed,
# switch_event_add_header_string, switch_event_fire_detailed,
# switch_event_destroy). The CI builder container ships
# /usr/local/lib/libfreeswitch.so via the FS install. The audit code
# isn't EXERCISED by server_test (the test only drives Health), but
# the linker still needs to resolve the symbols.
#
# Link both -L hint + -lfreeswitch directly so the link works under
# OSW_ENABLE_ASAN=OFF too (the previous find_library indirection silently
# no-op'd for the static-analysis job because find_library's cache
# decided FREESWITCH_LIBRARY-NOTFOUND).
target_link_directories(server_test PRIVATE /usr/local/lib)
target_link_libraries(server_test PRIVATE freeswitch)
