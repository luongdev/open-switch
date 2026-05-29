# tests/integration/CMakeLists.txt
#
# W7 integration coverage is still in-process: it reuses the handler test
# binaries that stand up a real gRPC MediaBridge server and exercise control,
# media, audit, registry, and FS-mock boundaries together. The real
# FreeSWITCH-in-container suite should add more tests here instead of relying
# only on these registrations.

add_test(NAME w7_start_bot_integration COMMAND start_bot_handler_test)
set_tests_properties(w7_start_bot_integration PROPERTIES LABELS "integration;w7d")

add_test(NAME w7_recording_relay_integration COMMAND start_recording_relay_handler_test)
set_tests_properties(w7_recording_relay_integration PROPERTIES LABELS "integration;w7b")
