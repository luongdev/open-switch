# tests/integration/CMakeLists.txt
#
# W7 integration coverage uses dedicated in-process binaries that cross the
# control callback, media relay, gRPC StreamClient, and FS-mock seams. A
# real FreeSWITCH-in-container harness should extend this suite when it lands.

add_executable(w7_media_flow_integration_test
    w7_media_flow_integration_test.cc
)
target_link_libraries(w7_media_flow_integration_test PRIVATE
    gtest_main
    osw_control
    osw_control_media_streaming_test_helpers
    osw_media
    osw_media_test_helpers
)
target_include_directories(w7_media_flow_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(w7_media_flow_integration_test PRIVATE
    OSW_TEST_FS_MOCK=1
)
target_compile_options(w7_media_flow_integration_test PRIVATE
    -Wno-error
)

add_test(
    NAME w7_streaming_callbacks_integration
    COMMAND w7_media_flow_integration_test
            --gtest_filter=W7MediaFlowIntegrationTest.StreamingCallbacksResampleBothDirections
)
set_tests_properties(w7_streaming_callbacks_integration PROPERTIES LABELS "integration;w6c;w7d")

add_test(
    NAME w7_recording_relay_media_integration
    COMMAND w7_media_flow_integration_test
            --gtest_filter=W7MediaFlowIntegrationTest.RecordingRelayMonoUsesWriteSideAndStereoInterleaves
)
set_tests_properties(w7_recording_relay_media_integration PROPERTIES LABELS "integration;w7b")
