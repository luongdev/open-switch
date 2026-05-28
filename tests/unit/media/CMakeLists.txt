# tests/unit/media/CMakeLists.txt — W6 media plane unit tests (Tracks A + B).
#
# Four test binaries:
#
#   bug_manager_test   — Track A; uses OSW_TEST_FS_MOCK=1 against the
#                        osw_media_test_helpers seam (no real libfreeswitch).
#   audio_frame_test   — Track B; FS-agnostic, links osw_media + osw_proto.
#   resampler_test     — Track B; links osw_media_fs (wraps
#                        switch_audio_resampler_t). Needs libfreeswitch.so
#                        (available in builder container).
#   stream_client_test — Track B; links osw_media + osw_proto; uses an
#                        in-process gRPC server.

# --- bug_manager_test (Track A, FS-mock seam) --------------------------

osw_add_unit_test(
    NAME    bug_manager_test
    LABEL   "media;unit;w6a"
    SOURCES bug_manager_test.cc
    # osw_media comes first so linker accumulates its dependencies before
    # scanning osw_media_test_helpers (same discipline as W3A control tests).
    LIBS    osw_media osw_media_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
# gmock for EXPECT_THAT + HasSubstr matchers.
target_link_libraries(bug_manager_test PRIVATE gmock)

# --- silence_driver_test (W6.6, FS-mock seam) --------------------------

osw_add_unit_test(
    NAME    silence_driver_test
    LABEL   "media;unit;w6.6"
    SOURCES silence_driver_test.cc
    LIBS    osw_media osw_media_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)

# --- audio_frame_test (Track B, FS-agnostic) ---------------------------

osw_add_unit_test(
    NAME    audio_frame_test
    LABEL   "media;unit"
    SOURCES audio_frame_test.cc
    LIBS    osw_media
)

# --- resampler_test (Track B, requires libfreeswitch + spandsp) --------
#
# Links osw_media_fs which includes resampler.cc compiled against
# <switch_resample.h>. The INTERFACE libfreeswitch dep on osw_media_fs
# pulls in the real switch_resample_* symbols.

osw_add_unit_test(
    NAME    resampler_test
    LABEL   "media;unit"
    SOURCES resampler_test.cc
    LIBS    osw_media_fs
)

# --- stream_client_test (Track B, requires gRPC runtime) ---------------

osw_add_unit_test(
    NAME    stream_client_test
    LABEL   "media;unit"
    SOURCES stream_client_test.cc
    LIBS    osw_media
)

# --- tts_playout_buffer_test (Track C) ---------------------------------
#
# TtsPlayoutBuffer lives in osw_media_fs (uses osw::audit::Emit which calls
# switch_event_fire). The test links osw_media_tts_test_helpers which
# compiles tts_playout_buffer.cc against osw_audit_test_helpers (stubs
# switch_event_fire), avoiding the need for a real libfreeswitch.

osw_add_unit_test(
    NAME    tts_playout_buffer_test
    LABEL   "media;unit;w6c"
    SOURCES tts_playout_buffer_test.cc
    LIBS    osw_media osw_media_tts_test_helpers
)

# --- bug_fanout_test (W7 Track D, pure unit) ---------------------------

osw_add_unit_test(
    NAME    bug_fanout_test
    LABEL   "media;unit;w7d"
    SOURCES bug_fanout_test.cc
    LIBS    osw_media
)
