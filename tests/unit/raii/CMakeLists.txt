# tests/unit/raii/CMakeLists.txt
#
# All RAII helper tests build with -DOSW_TEST_FS_MOCK=1 (the FS-mock
# seam — see tests/unit/raii/README.md). They link nothing from the
# production module .so; the RAII headers are header-only.

osw_add_unit_test(
    NAME    session_lock_test
    LABEL   "raii;unit"
    SOURCES session_lock_test.cc
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    event_guard_test
    LABEL   "raii;unit"
    SOURCES event_guard_test.cc
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    media_bug_lease_test
    LABEL   "raii;unit"
    SOURCES media_bug_lease_test.cc
    DEFINES OSW_TEST_FS_MOCK=1
)

osw_add_unit_test(
    NAME    xml_node_test
    LABEL   "raii;unit"
    SOURCES xml_node_test.cc
    DEFINES OSW_TEST_FS_MOCK=1
)
