# tests/CMakeLists.txt — top-level test wiring.
#
# Enabled when -DOSW_BUILD_TESTS=ON. Uses GoogleTest fetched as a
# CMake-FetchContent dependency (vendored at configure time so the
# builder image stays slim — no apt-package gtest needed).
#
# Three test categories registered with CTest labels:
#
#   raii          - Header-only RAII helper tests, FS-mock seam.
#                   Cheap, run on every PR.
#   unit          - Per-subsystem unit tests (config, log, health,
#                   server start/stop). FS-mock seam where applicable.
#   integration   - Cross-subsystem integration tests. Currently empty;
#                   populated in W5 when a real-FS harness exists.
#
# ASAN + LSAN gating is inherited from the root CMakeLists.txt's
# OSW_ENABLE_ASAN option. The CI workflow already runs ctest -L unit
# and ctest -L integration under ASAN; see .github/workflows/ci.yml.

include(FetchContent)

# Pin to a known-good GoogleTest tag. v1.14.0 is the latest stable as
# of the W1 implementation date; bump deliberately when needed.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Don't install gtest into /usr/local on `make install`.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# GoogleTest's headers and main get the project compile options. Its
# sources do NOT need -Werror; tag the targets to clear strict warnings.
foreach(_gt_tgt gtest gtest_main gmock gmock_main)
    if(TARGET ${_gt_tgt})
        target_compile_options(${_gt_tgt} PRIVATE
            -Wno-error
            -Wno-deprecated-declarations
            -Wno-conversion
        )
    endif()
endforeach()

# Helper that registers a unit-test binary with the right defines + libs.
# Usage:
#   osw_add_unit_test(NAME session_lock_test
#                     LABEL raii
#                     SOURCES tests/unit/raii/session_lock_test.cc
#                     LIBS    osw_raii_test_helpers)
#
function(osw_add_unit_test)
    # LABEL is multi-value so callers can pass `LABEL "raii;unit"` —
    # CMake expands semicolons inside quoted args into a 2-element list
    # before the function call, so a single-value LABEL would silently
    # drop the second label (causing `ctest -L unit` to find 0 tests
    # when callers expect a hierarchical label).
    cmake_parse_arguments(_TST "" "NAME" "LABEL;SOURCES;LIBS;DEFINES" ${ARGN})
    if(NOT _TST_NAME OR NOT _TST_SOURCES)
        message(FATAL_ERROR "osw_add_unit_test requires NAME and SOURCES")
    endif()
    if(NOT _TST_LABEL)
        set(_TST_LABEL "unit")
    endif()
    # Ensure every test carries the "unit" label so `ctest -L unit`
    # picks it up even if the caller specified only a subsystem label.
    list(APPEND _TST_LABEL "unit")
    list(REMOVE_DUPLICATES _TST_LABEL)

    add_executable(${_TST_NAME} ${_TST_SOURCES})
    target_link_libraries(${_TST_NAME} PRIVATE
        gtest_main
        ${_TST_LIBS}
    )
    target_include_directories(${_TST_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_definitions(${_TST_NAME} PRIVATE ${_TST_DEFINES})
    # Don't apply project -Werror to test code (gtest macros generate
    # warnings under some clang versions). Tests still run under ASAN.
    target_compile_options(${_TST_NAME} PRIVATE
        -Wno-error
    )

    add_test(NAME ${_TST_NAME} COMMAND ${_TST_NAME})
    set_tests_properties(${_TST_NAME} PROPERTIES LABELS "${_TST_LABEL}")
endfunction()

add_subdirectory(unit)
