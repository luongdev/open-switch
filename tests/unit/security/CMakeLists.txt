# tests/unit/security/CMakeLists.txt

osw_add_unit_test(
    NAME    eavesdrop_policy_test
    LABEL   "security;unit;w7a"
    SOURCES eavesdrop_policy_test.cc
    LIBS    osw_security
)

osw_add_unit_test(
    NAME    eavesdrop_marker_test
    LABEL   "security;unit;w7a"
    SOURCES eavesdrop_marker_test.cc
    LIBS    osw_security_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    eavesdrop_app_test
    LABEL   "security;unit;w7a"
    SOURCES eavesdrop_app_test.cc
    LIBS    osw_security_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    eavesdrop_detector_test
    LABEL   "security;unit;w7a"
    SOURCES eavesdrop_detector_test.cc
    LIBS    osw_security_test_helpers
    DEFINES OSW_TEST_FS_MOCK=1
)
