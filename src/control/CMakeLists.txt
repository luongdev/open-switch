# src/control/CMakeLists.txt — gRPC control plane skeleton.
#
# Two libraries:
#
#   osw_control      — FS-agnostic. GrpcServer + ControlServiceSkeleton +
#                      Health handler (real impl) + UNIMPLEMENTED stubs
#                      for the rest. Links osw_proto (which carries gRPC
#                      + protobuf transitively). Tests link this directly.
#
#   osw_control_fs   — FS-dependent control handlers. Currently holds
#                      the SubscribeEvents handler (it calls
#                      osw::audit::Emit, which lives in
#                      osw_observability_fs because it invokes
#                      switch_event_fire). Linked into the module .so
#                      only. W3 control handlers (Originate, Hangup, ...)
#                      will land here too.

add_library(osw_control STATIC
    server.cc
    tls.cc
    # W4 Track A — TLS + cert reloader (opt-in mTLS; default off)
    config_tls.cc
    tls_reloader.cc
    # W4 Track C — RPC metrics collector + interceptor
    rpc_metrics.cc
    # W5 Track B — idempotency cache (FS-agnostic; lives in osw_control so
    # tests can link it without pulling in the FS-dependent handlers library)
    idempotency_cache.cc
    handlers/health_handler.cc
    handlers/unimplemented.cc
)
target_include_directories(osw_control
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}                  # for "src/control/..." includes
        ${OSW_PROTO_GEN_DIR}                 # for "open_switch/.../*.pb.h"
)
target_compile_features(osw_control PUBLIC cxx_std_20)
target_link_libraries(osw_control PUBLIC
    osw_events
    osw_observability
    osw_proto
    Threads::Threads
    # W4 Track B: JWT verifier uses OpenSSL EVP / ECDSA
    # (already linked via gRPC's BoringSSL on most platforms; explicitly
    # list OpenSSL::Crypto as a fallback for dev builds without BoringSSL).
    OpenSSL::Crypto
)
set_target_properties(osw_control PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_control_fs (FS-dependent handlers) ---------------------------
add_library(osw_control_fs STATIC
    handlers/subscribe_events_handler.cc
    # W3 Track A — lifecycle RPCs + shared helpers
    call_cause.cc
    session_guard.cc
    originate_options.cc
    var_denylist.cc
    handlers/originate_handler.cc
    handlers/hangup_handler.cc
    handlers/hangup_many_handler.cc
    # W3 Track B — connect/execute RPCs
    handlers/bridge_handler.cc
    handlers/execute_handler.cc
    handlers/blind_transfer_handler.cc
    # W3 Track C — channel mutators (FF-026..027)
    handlers/set_variables_handler.cc
    handlers/hold_handler.cc
    handlers/unhold_handler.cc
    # W6 Track C — media streaming RPCs + bug callbacks
    active_media_streams.cc
    handlers/media_bug_callbacks.cc
    handlers/start_tts_handler.cc
    handlers/start_stt_handler.cc
    handlers/start_voicebot_handler.cc
    handlers/stop_media_stream_handler.cc
)
target_include_directories(osw_control_fs
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}                  # for "src/control/..." includes
        ${OSW_PROTO_GEN_DIR}                 # for "open_switch/.../*.pb.h"
)
target_compile_features(osw_control_fs PUBLIC cxx_std_20)
target_link_libraries(osw_control_fs PUBLIC
    osw_control
    osw_events_fs                            # for GenerateUuidV7 (envelope.cc) + FS event binder
    osw_media_fs                             # for MediaBugManager, TtsPlayoutBuffer (Track C)
    osw_observability
    osw_observability_fs                     # for osw::audit::Emit
    osw_proto
    Threads::Threads
)
set_target_properties(osw_control_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_control_call_lifecycle_test_helpers (FS-mock seam for W3 Track A tests) --
#
# Compiles the W3A helpers and handlers against OSW_TEST_FS_MOCK=1 so
# unit tests can run without a real FreeSWITCH library. Mirrors the
# pattern from src/observability/CMakeLists.txt (osw_audit_test_helpers).
# Never linked into the production mod_open_switch.so.
if(OSW_BUILD_TESTS)
    # After W3 (A+B+C) all 9 ControlServiceSkeleton method overrides live
    # in dedicated handler TUs and unimplemented.cc carries NO method
    # bodies. To keep each test binary's vtable complete the helper libs
    # below each compile EVERY W3 handler under OSW_TEST_FS_MOCK=1 (the
    # other-track handlers happen to also exercise the mock seam cleanly).
    # Plus subscribe_events_stub for the W2 SubscribeEvents method.
    add_library(osw_control_call_lifecycle_test_helpers STATIC
        call_cause.cc
        session_guard.cc
        originate_options.cc
        var_denylist.cc
        handlers/originate_handler.cc
        handlers/hangup_handler.cc
        handlers/hangup_many_handler.cc
        # W3 Track B handlers — included for vtable completeness so
        # tests linking only osw_control_call_lifecycle_test_helpers still resolve
        # Bridge/Execute/BlindTransfer overrides.
        handlers/bridge_handler.cc
        handlers/execute_handler.cc
        handlers/blind_transfer_handler.cc
        # W3 Track C — channel mutators (FF-026..027)
        handlers/set_variables_handler.cc
        handlers/hold_handler.cc
        handlers/unhold_handler.cc
        # W6 Track C — media streaming stubs for vtable completeness
        active_media_streams.cc
        handlers/media_bug_callbacks.cc
        handlers/start_tts_handler.cc
        handlers/start_stt_handler.cc
        handlers/start_voicebot_handler.cc
        handlers/stop_media_stream_handler.cc
        # Stub for SubscribeEvents so the test binary vtable is complete
        # without pulling in the real FS-dependent subscribe_events_handler.cc.
        handlers/subscribe_events_stub.cc
    )
    target_include_directories(osw_control_call_lifecycle_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_include_directories(osw_control_call_lifecycle_test_helpers PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OSW_PROTO_GEN_DIR}
    )
    target_compile_features(osw_control_call_lifecycle_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_control_call_lifecycle_test_helpers PUBLIC
        osw_control
        osw_events_test_helpers
        osw_media
        osw_media_tts_test_helpers
        osw_observability
        osw_audit_test_helpers
        osw_media_test_helpers
        osw_proto
        Threads::Threads
    )
    target_compile_definitions(osw_control_call_lifecycle_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_control_call_lifecycle_test_helpers PRIVATE
        -Wno-error
    )

    # --- osw_control_call_routing_test_helpers (FS-mock seam for W3 Track B tests) --
    #
    # Extends the W3A helpers with the Track B handlers (bridge, execute,
    # blind_transfer). Uses the same OSW_TEST_FS_MOCK seam. Track C
    # handlers (set_variables, hold, unhold) are also included for vtable
    # completeness — without them, tests linking only this lib fail at
    # link time on missing SetVariables/Hold/Unhold overrides.
    add_library(osw_control_call_routing_test_helpers STATIC
        call_cause.cc
        session_guard.cc
        originate_options.cc
        var_denylist.cc
        handlers/originate_handler.cc
        handlers/hangup_handler.cc
        handlers/hangup_many_handler.cc
        handlers/bridge_handler.cc
        handlers/execute_handler.cc
        handlers/blind_transfer_handler.cc
        # W3 Track C handlers — included for vtable completeness.
        handlers/set_variables_handler.cc
        handlers/hold_handler.cc
        handlers/unhold_handler.cc
        # W6 Track C handlers — included for vtable completeness.
        active_media_streams.cc
        handlers/media_bug_callbacks.cc
        handlers/start_tts_handler.cc
        handlers/start_stt_handler.cc
        handlers/start_voicebot_handler.cc
        handlers/stop_media_stream_handler.cc
        # Stub for SubscribeEvents (same rationale as w3a helpers).
        handlers/subscribe_events_stub.cc
    )
    target_include_directories(osw_control_call_routing_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_include_directories(osw_control_call_routing_test_helpers PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OSW_PROTO_GEN_DIR}
    )
    target_compile_features(osw_control_call_routing_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_control_call_routing_test_helpers PUBLIC
        osw_control
        osw_events_test_helpers
        osw_media
        osw_media_tts_test_helpers
        osw_observability
        osw_audit_test_helpers
        osw_media_test_helpers
        osw_proto
        Threads::Threads
    )
    target_compile_definitions(osw_control_call_routing_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_control_call_routing_test_helpers PRIVATE
        -Wno-error
    )

    # --- osw_control_media_streaming_test_helpers (FS-mock seam for W6 Track C tests) --
    #
    # Compiles all W6C handlers and the ActiveMediaStreams registry against
    # OSW_TEST_FS_MOCK=1 so unit tests run without a real FreeSWITCH library.
    # Includes every ControlServiceSkeleton override so handler test binaries
    # have a complete vtable.
    add_library(osw_control_media_streaming_test_helpers STATIC
        call_cause.cc
        session_guard.cc
        originate_options.cc
        var_denylist.cc
        handlers/originate_handler.cc
        handlers/hangup_handler.cc
        handlers/hangup_many_handler.cc
        handlers/bridge_handler.cc
        handlers/execute_handler.cc
        handlers/blind_transfer_handler.cc
        handlers/set_variables_handler.cc
        handlers/hold_handler.cc
        handlers/unhold_handler.cc
        active_media_streams.cc
        handlers/media_bug_callbacks.cc
        handlers/start_tts_handler.cc
        handlers/start_stt_handler.cc
        handlers/start_voicebot_handler.cc
        handlers/stop_media_stream_handler.cc
        handlers/subscribe_events_stub.cc
    )
    target_include_directories(osw_control_media_streaming_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_include_directories(osw_control_media_streaming_test_helpers PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OSW_PROTO_GEN_DIR}
    )
    target_compile_features(osw_control_media_streaming_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_control_media_streaming_test_helpers PUBLIC
        osw_control
        osw_events_test_helpers
        osw_media
        osw_media_tts_test_helpers
        osw_observability
        osw_audit_test_helpers
        osw_media_test_helpers
        osw_proto
        Threads::Threads
    )
    target_compile_definitions(osw_control_media_streaming_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_control_media_streaming_test_helpers PRIVATE
        -Wno-error
    )
    set_target_properties(osw_control_media_streaming_test_helpers PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()

