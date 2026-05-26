# tests/unit/observability/CMakeLists.txt
#
# Tests for osw::log and osw::Health. These link osw_observability
# (FS-agnostic). They do NOT link osw_observability_fs — the test
# installs its own log sink via osw::log::SetSinkForTesting so the
# default switch_log_printf path is never reached.

osw_add_unit_test(
    NAME    log_test
    LABEL   "observability;unit"
    SOURCES log_test.cc
    LIBS    osw_observability
)

osw_add_unit_test(
    NAME    health_test
    LABEL   "observability;unit"
    SOURCES health_test.cc
    LIBS    osw_observability
)

# Audit helper test — links the FS-mock-seam variant of audit.cc
# (osw_audit_test_helpers, defined in src/observability/CMakeLists.txt).
# OSW_TEST_FS_MOCK is forwarded by that lib's PUBLIC compile-definition
# so the test TU pulls in fs_mock.h instead of <switch.h>.
osw_add_unit_test(
    NAME    audit_test
    LABEL   "observability;unit"
    SOURCES audit_test.cc
    LIBS    osw_audit_test_helpers osw_observability
)
