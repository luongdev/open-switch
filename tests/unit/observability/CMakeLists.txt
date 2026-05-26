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
