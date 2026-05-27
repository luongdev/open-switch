# src/media/CMakeLists.txt — W6 media plane (Track B: AudioFrame + Resampler + StreamClient)
#
# Two libraries:
#
#   osw_media        — FS-agnostic: AudioFrame + StreamClient.
#                      StreamClient only depends on osw_proto (gRPC stubs) +
#                      osw_observability (log). No <switch.h>.
#                      Linked by tests + the production .so.
#
#   osw_media_fs     — FS-dependent: Resampler (wraps switch_audio_resampler_t).
#                      Includes <switch_resample.h>. Linked into the .so only.
#                      Tests link against the real libfreeswitch.so because
#                      the builder image provides it.

# --- osw_media (FS-agnostic) ------------------------------------------

add_library(osw_media STATIC
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
    osw_proto
    osw_observability
    Threads::Threads
)

set_target_properties(osw_media PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- osw_media_fs (FS-dependent: Resampler) ---------------------------

add_library(osw_media_fs STATIC
    resampler.cc
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
)

# link libfreeswitch for switch_resample_* symbols
find_library(FREESWITCH_LIBRARY NAMES freeswitch
             HINTS /usr/local/lib NO_CMAKE_FIND_ROOT_PATH)
if(FREESWITCH_LIBRARY)
    target_link_libraries(osw_media_fs INTERFACE ${FREESWITCH_LIBRARY})
else()
    target_link_options(osw_media_fs INTERFACE -lfreeswitch)
endif()

target_compile_definitions(osw_media_fs PRIVATE SWITCH_API_VISIBILITY=1)
set_target_properties(osw_media_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)
