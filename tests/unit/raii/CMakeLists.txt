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

# event_guard_test, media_bug_lease_test, xml_node_test land in
# subsequent commits.
