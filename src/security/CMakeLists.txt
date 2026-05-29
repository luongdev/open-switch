# src/security/CMakeLists.txt — W7 Track A eavesdrop policy stack.

add_library(osw_security STATIC
    eavesdrop_policy.cc
)
target_include_directories(osw_security PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_security PUBLIC cxx_std_20)
target_link_libraries(osw_security PUBLIC
    osw_observability
)
set_target_properties(osw_security PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(osw_security_fs STATIC
    eavesdrop_marker.cc
    eavesdrop_audit.cc
    eavesdrop_app.cc
    eavesdrop_detector.cc
)
target_include_directories(osw_security_fs PRIVATE
    ${OSW_FREESWITCH_INCLUDE_DIR}
)
target_include_directories(osw_security_fs PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(osw_security_fs PUBLIC cxx_std_20)
target_link_libraries(osw_security_fs PUBLIC
    osw_security
    osw_observability
    osw_observability_fs
)
target_compile_definitions(osw_security_fs PRIVATE SWITCH_API_VISIBILITY=1)
set_target_properties(osw_security_fs PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OSW_BUILD_TESTS)
    add_library(osw_security_test_helpers STATIC
        eavesdrop_marker.cc
        eavesdrop_audit.cc
        eavesdrop_app.cc
        eavesdrop_detector.cc
    )
    target_include_directories(osw_security_test_helpers PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_features(osw_security_test_helpers PUBLIC cxx_std_20)
    target_link_libraries(osw_security_test_helpers PUBLIC
        osw_security
        osw_observability
        osw_audit_test_helpers
    )
    target_compile_definitions(osw_security_test_helpers PUBLIC
        OSW_TEST_FS_MOCK=1
    )
    target_compile_options(osw_security_test_helpers PRIVATE
        -Wno-error
    )
    set_target_properties(osw_security_test_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
