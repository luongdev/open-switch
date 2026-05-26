# tests/unit/core/CMakeLists.txt
#
# Tests for FS-agnostic core (config validation, lifecycle). The
# FS-dependent loader (LoadConfigFromFile) + Module singleton are
# exercised by the W5 integration suite, not by unit tests.

osw_add_unit_test(
    NAME    config_test
    LABEL   "core;unit"
    SOURCES config_test.cc
    LIBS    osw_core
)

osw_add_unit_test(
    NAME    lifecycle_test
    LABEL   "core;unit"
    SOURCES lifecycle_test.cc
    LIBS    osw_core
)
