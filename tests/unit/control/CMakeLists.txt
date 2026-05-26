# tests/unit/control/CMakeLists.txt — gRPC server smoke tests.
#
# These tests need a real gRPC runtime. CI runs them in the builder
# container; on dev hosts without gRPC the test fails to link, which
# matches the contract's "if gRPC isn't installed locally, document
# the gap; CI runs in the builder container".

osw_add_unit_test(
    NAME    server_test
    LABEL   "control;unit"
    SOURCES server_test.cc
    LIBS    osw_control
)
