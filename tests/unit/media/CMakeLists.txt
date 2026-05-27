# tests/unit/media/CMakeLists.txt — W6 media plane unit tests.
#
# Three test binaries:
#
#   audio_frame_test   — FS-agnostic; links osw_media + osw_proto.
#   resampler_test     — Links osw_media_fs (which wraps switch_audio_resampler_t).
#                        Requires libfreeswitch.so (available in builder container).
#   stream_client_test — Links osw_media + osw_proto. Uses in-process gRPC server.
#                        Requires a real gRPC runtime (builder container).

# --- audio_frame_test (FS-agnostic) -------------------------------------

osw_add_unit_test(
    NAME    audio_frame_test
    LABEL   "media;unit"
    SOURCES audio_frame_test.cc
    LIBS    osw_media
)

# --- resampler_test (requires libfreeswitch + spandsp) ------------------
#
# Links osw_media_fs which includes resampler.cc compiled against
# <switch_resample.h>.  The INTERFACE libfreeswitch dep on osw_media_fs
# pulls in the real switch_resample_* symbols.

osw_add_unit_test(
    NAME    resampler_test
    LABEL   "media;unit"
    SOURCES resampler_test.cc
    LIBS    osw_media_fs
)

# --- stream_client_test (requires gRPC runtime) -------------------------

osw_add_unit_test(
    NAME    stream_client_test
    LABEL   "media;unit"
    SOURCES stream_client_test.cc
    LIBS    osw_media
)
