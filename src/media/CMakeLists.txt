# src/media/CMakeLists.txt — W6 media plane subsystem (Tracks A + B).
#
# Two libraries + one test-helper:
#
#   osw_media          — FS-agnostic. Holds:
#                          * Track A: Purpose + BugHandle (move-only RAII lease)
#                          * Track B: AudioFrame + StreamClient (bidi gRPC client
#                                     over open_switch::media::v1::MediaBridge)
#                        Links osw_proto (gRPC + protobuf for grpc::StatusCode
#                        + the generated stub) + osw_observability (log).
#                        Tests link this directly.
#
#   osw_media_fs       — FS-dependent. Holds:
#                          * Track A: MediaBugManager (switch_core_media_bug_add /
#                                     _remove_callback)
#                          * Track B: Resampler (wraps switch_audio_resampler_t
#                                     via <switch_resample.h>; needs libfreeswitch
#                                     at link time)
#                        Linked into the production .so only.
#
#   osw_media_test_helpers — FS-mock seam for the bug_manager_test
#                            (compiles bug_manager.cc against
#                            OSW_TEST_FS_MOCK=1).  Never linked into the .so.

# --- osw_media (FS-agnostic) -------------------------------------------

add_library(osw_media STATIC
    # Track A
    purpose.cc
    bug_handle.cc
    # Track B
    audio_frame.cc
    stream_client.cc
)
target_include_directories(osw_media
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${OSW_PROTO_GEN_DIR}
)
target_compile_features(osw_media PUBLIC cxx_std_20)
target_link_libraries(osw_media PUBLIC
    osw_proto         # grpc::StatusCode + generated MediaBridge stub
    osw_observability # log
    Threads::Threads
)
set_target_properties(osw_media PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_media_fs (FS-dependent) ---------------------------------------

add_library(osw_media_fs STATIC
    # Track A
    bug_manager.cc
    # Track B
    resampler.cc
    # Track C
    tts_playout_buffer.cc
)
target_include_directories(osw_media_fs
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${OSW_FREESWITCH_INCLUDE_DIR}
)
target_compile_features(osw_media_fs PUBLIC cxx_std_20)
target_link_libraries(osw_media_fs PUBLIC
    osw_media
    osw_observability
    osw_observability_fs   # tts_playout_buffer.cc calls osw::audit::Emit
    Threads::Threads
)

# Link libfreeswitch for switch_resample_* + switch_core_media_bug_* symbols
# (Track B's Resampler and Track A's MediaBugManager both need it).
find_library(FREESWITCH_LIBRARY NAMES freeswitch
             HINTS /usr/local/lib NO_CMAKE_FIND_ROOT_PATH)
if(FREESWITCH_LIBRARY)
    target_link_libraries(osw_media_fs INTERFACE ${FREESWITCH_LIBRARY})
else()
    target_link_options(osw_media_fs INTERFACE -lfreeswitch)
endif()

target_compile_definitions(osw_media_fs PRIVATE SWITCH_API_VISIBILITY=1)
set_target_properties(osw_media_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_media_test_helpers (FS-mock seam for W6A unit tests) ----------

if(OSW_BUILD_TESTS)
    add_library(osw_media_test_helpers STATIC
        bug_manager.cc
    )
    target_include_directories(osw_media_test_helpers
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}
    )
    target_compile_features(osw_media_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_media_test_helpers PUBLIC
        osw_media
        Threads::Threads
    )
    target_compile_definitions(osw_media_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_media_test_helpers PRIVATE
        -Wno-error
    )
    set_target_properties(osw_media_test_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # osw_media_tts_test_helpers — compiles TtsPlayoutBuffer against the
    # osw_audit_test_helpers stub (no libfreeswitch needed). Used by
    # tts_playout_buffer_test which is FS-agnostic apart from audit calls.
    add_library(osw_media_tts_test_helpers STATIC
        tts_playout_buffer.cc
    )
    target_include_directories(osw_media_tts_test_helpers
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}
    )
    target_compile_features(osw_media_tts_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_media_tts_test_helpers PUBLIC
        osw_media
        osw_audit_test_helpers
        Threads::Threads
    )
    target_compile_options(osw_media_tts_test_helpers PRIVATE
        -Wno-error
    )
    set_target_properties(osw_media_tts_test_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
