# tests/unit/control/CMakeLists.txt — gRPC control-plane unit tests.
#
# server_test: gRPC server smoke test. Needs a real gRPC runtime. CI
# runs in the builder container; on dev hosts without gRPC the target
# fails to link (documented gap — CI is the source of truth).
#
# W3A tests (call_cause_test, originate_handler_test, hangup_handler_test,
# hangup_many_handler_test): run against the FS-mock seam via
# osw_control_call_lifecycle_test_helpers. No real FS library required.
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

# W4 Track A — TLS credential builder tests.
#
# Tests BuildServerCredentials(TlsConfig), MakeServerCreds(Config) adapter,
# and TlsConfigFromConfig(). All use in-memory PEM strings written to a
# temp directory. No real TLS handshake is performed (FF-028 §unit tests).
# Links osw_control (FS-agnostic; no FS-mock seam needed).
osw_add_unit_test(
    NAME    tls_test
    LABEL   "control;unit;tls"
    SOURCES tls_test.cc
    LIBS    osw_control
)

# W3 Track A — shared helpers + handler tests (FS-mock seam).
#
# The handler tests include "src/control/control_service_skeleton.h" via
# its project-relative path. osw_add_unit_test only adds
# ${CMAKE_SOURCE_DIR}/include; we must add ${CMAKE_SOURCE_DIR} as well so
# that "src/..." includes resolve from the project root. The generated
# proto headers live under ${OSW_PROTO_GEN_DIR} and are already in scope
# via the transitive PUBLIC include of osw_control (which links osw_proto).

osw_add_unit_test(
    NAME    call_cause_test
    LABEL   "control;unit"
    SOURCES call_cause_test.cc
    LIBS    osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    originate_handler_test
    LABEL   "control;unit"
    SOURCES originate_handler_test.cc
    # osw_control FIRST: the vtable in osw_control's health_handler.cc.o has
    # forward references to all ControlServiceSkeleton overrides. Listing
    # osw_control first lets the linker accumulate the unresolved symbols
    # before scanning osw_control_call_lifecycle_test_helpers (which provides the W3A
    # overrides + SubscribeEvents stub). Same linker-order discipline as
    # server_test (osw_control osw_control_fs).
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(originate_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    hangup_handler_test
    LABEL   "control;unit"
    SOURCES hangup_handler_test.cc
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(hangup_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    hangup_many_handler_test
    LABEL   "control;unit"
    SOURCES hangup_many_handler_test.cc
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(hangup_many_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

# W3 Track B — connect/execute handler tests (FS-mock seam).
#
# All three tests link osw_control_call_routing_test_helpers which compiles the W3B
# handlers (bridge, execute, blind_transfer) against OSW_TEST_FS_MOCK=1.
# Link order: osw_control FIRST so vtable forward-references accumulate
# before the helpers library is scanned (same discipline as W3A tests).

osw_add_unit_test(
    NAME    bridge_handler_test
    LABEL   "control;unit"
    SOURCES bridge_handler_test.cc
    LIBS    osw_control osw_control_call_routing_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(bridge_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    execute_handler_test
    LABEL   "control;unit"
    SOURCES execute_handler_test.cc
    LIBS    osw_control osw_control_call_routing_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(execute_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    blind_transfer_handler_test
    LABEL   "control;unit"
    SOURCES blind_transfer_handler_test.cc
    LIBS    osw_control osw_control_call_routing_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(blind_transfer_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

# W3 Track C — channel mutators (FF-026..027). Reuses
# osw_control_call_lifecycle_test_helpers, which the Track C src/control/CMakeLists.txt
# extension compiles WITH the Track C handler TUs added in.
osw_add_unit_test(
    NAME    set_variables_handler_test
    LABEL   "control;unit"
    SOURCES set_variables_handler_test.cc
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(set_variables_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    hold_handler_test
    LABEL   "control;unit"
    SOURCES hold_handler_test.cc
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(hold_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    unhold_handler_test
    LABEL   "control;unit"
    SOURCES unhold_handler_test.cc
    LIBS    osw_control osw_control_call_lifecycle_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(unhold_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

# W4 Track C — RpcMetrics unit test. Links osw_control (which now includes
# rpc_metrics.cc) and osw_observability (for prometheus::Registry).
# Uses a real gRPC runtime only for the grpc::StatusCode enum — the test
# does NOT start a gRPC server, just calls Record/RecordAuthz directly.
osw_add_unit_test(
    NAME    rpc_metrics_test
    LABEL   "control;unit;w4c"
    SOURCES rpc_metrics_test.cc
    LIBS    osw_control osw_observability
)

# W5 Track B — IdempotencyCache unit test.
#
# Links osw_control (which includes idempotency_cache.cc as of W5B).
# No FS dependency; no FS-mock seam required.
# The concurrent stress test is designed to be TSAN-clean; CI runs the
# "control;unit" label under TSAN to verify.
osw_add_unit_test(
    NAME    idempotency_cache_test
    LABEL   "control;unit;w5b"
    SOURCES idempotency_cache_test.cc
    LIBS    osw_control
)

# W6 Track C — media streaming handler tests (FS-mock seam).
#
# All four tests link osw_control_media_streaming_test_helpers, which compiles the W6C
# handlers (start_tts, start_stt, start_voicebot, stop_media_stream) and
# ActiveMediaStreams against OSW_TEST_FS_MOCK=1.
# Link order: osw_control FIRST so vtable forward-references accumulate
# before the helpers library is scanned (same discipline as all other
# handler tests in this file).

osw_add_unit_test(
    NAME    start_stt_handler_test
    LABEL   "control;unit;w6c"
    SOURCES start_stt_handler_test.cc
    LIBS    osw_control osw_control_media_streaming_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(start_stt_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    start_tts_handler_test
    LABEL   "control;unit;w6c"
    SOURCES start_tts_handler_test.cc
    LIBS    osw_control osw_control_media_streaming_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(start_tts_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    start_voicebot_handler_test
    LABEL   "control;unit;w6c"
    SOURCES start_voicebot_handler_test.cc
    LIBS    osw_control osw_control_media_streaming_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(start_voicebot_handler_test PRIVATE ${CMAKE_SOURCE_DIR})

osw_add_unit_test(
    NAME    stop_media_stream_handler_test
    LABEL   "control;unit;w6c"
    SOURCES stop_media_stream_handler_test.cc
    LIBS    osw_control osw_control_media_streaming_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
target_include_directories(stop_media_stream_handler_test PRIVATE ${CMAKE_SOURCE_DIR})
