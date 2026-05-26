# cmake/proto.cmake — Proto + gRPC C++ codegen wiring.
#
# Generates:
#   ${OSW_PROTO_GEN_DIR}/open_switch/control/v1/control.pb.{h,cc}
#   ${OSW_PROTO_GEN_DIR}/open_switch/control/v1/control.grpc.pb.{h,cc}
#   ${OSW_PROTO_GEN_DIR}/open_switch/events/v1/events.pb.{h,cc}
#   ${OSW_PROTO_GEN_DIR}/open_switch/events/v1/events.grpc.pb.{h,cc}
#   ${OSW_PROTO_GEN_DIR}/open_switch/media/v1/media.pb.{h,cc}
#   ${OSW_PROTO_GEN_DIR}/open_switch/media/v1/media.grpc.pb.{h,cc}
#
# The generated sources are compiled into the internal static library
# `osw_proto`, which the module target (`mod_open_switch`) consumes
# privately. Because the .so links protobuf + gRPC statically (the
# builder image installs them under /opt/grpc), the generated .o files
# and the C++ runtime libs all live inside our .so, isolated from any
# other module by `-Wl,--exclude-libs,ALL` + FS's RTLD_LOCAL loader
# (FREESWITCH-FACTS FF-010).
#
# Buf-lint integration:
#   - `osw_proto_lint` shells out to `buf lint` on `${OSW_PROTO_DIR}`
#     if `buf` is on PATH. Non-blocking when absent (so OS-package
#     builds without buf still succeed; CI uses bufbuild/buf-action
#     in a separate workflow step regardless).
#
# Generated headers are NEVER edited by hand; they sit under
# ${CMAKE_BINARY_DIR}/proto-gen and are excluded from clang-tidy.

# --- Discover the gRPC C++ codegen plugin -----------------------------
# gRPC CMake config exports `gRPC::grpc_cpp_plugin`. If we can find that
# target, use its location; otherwise fall back to `grpc_cpp_plugin` on
# PATH.
if(TARGET gRPC::grpc_cpp_plugin)
    set(_osw_grpc_cpp_plugin "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
else()
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    set(_osw_grpc_cpp_plugin "${GRPC_CPP_PLUGIN}")
endif()

# protoc itself — the Protobuf CMake config exports `protobuf::protoc`.
if(TARGET protobuf::protoc)
    set(_osw_protoc "$<TARGET_FILE:protobuf::protoc>")
else()
    find_program(PROTOC_BIN protoc REQUIRED)
    set(_osw_protoc "${PROTOC_BIN}")
endif()

message(STATUS "  protoc:            ${_osw_protoc}")
message(STATUS "  grpc_cpp_plugin:   ${_osw_grpc_cpp_plugin}")

# --- Enumerate proto inputs --------------------------------------------
# The protos sit under proto/open_switch/{control,events,media}/v1/.
# Each .proto produces 4 generated files.
set(_osw_protos
    "${OSW_PROTO_DIR}/open_switch/control/v1/control.proto"
    "${OSW_PROTO_DIR}/open_switch/events/v1/events.proto"
    "${OSW_PROTO_DIR}/open_switch/media/v1/media.proto"
)

set(_osw_proto_outputs "")

foreach(proto_in IN LISTS _osw_protos)
    # Compute the relative path so the generated tree mirrors proto/.
    file(RELATIVE_PATH proto_rel "${OSW_PROTO_DIR}" "${proto_in}")
    string(REGEX REPLACE "\\.proto$" "" proto_stem "${proto_rel}")

    set(_pb_h    "${OSW_PROTO_GEN_DIR}/${proto_stem}.pb.h")
    set(_pb_cc   "${OSW_PROTO_GEN_DIR}/${proto_stem}.pb.cc")
    set(_grpc_h  "${OSW_PROTO_GEN_DIR}/${proto_stem}.grpc.pb.h")
    set(_grpc_cc "${OSW_PROTO_GEN_DIR}/${proto_stem}.grpc.pb.cc")

    add_custom_command(
        OUTPUT  "${_pb_h}" "${_pb_cc}" "${_grpc_h}" "${_grpc_cc}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSW_PROTO_GEN_DIR}"
        COMMAND
            "${_osw_protoc}"
                --proto_path=${OSW_PROTO_DIR}
                --cpp_out=${OSW_PROTO_GEN_DIR}
                --grpc_out=${OSW_PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${_osw_grpc_cpp_plugin}
                "${proto_in}"
        DEPENDS "${proto_in}"
        COMMENT "protoc ${proto_rel}"
        VERBATIM
    )

    list(APPEND _osw_proto_outputs "${_pb_cc}" "${_grpc_cc}")
endforeach()

# --- Static library that holds the generated code ---------------------
add_library(osw_proto STATIC ${_osw_proto_outputs})

# Generated code uses non-aggressive defaults; loosen the project's
# strict warnings for this target only. The codegen is upstream-owned.
target_compile_options(osw_proto PRIVATE
    -Wno-unused-parameter
    -Wno-deprecated-declarations
    -Wno-conversion
    -Wno-sign-compare
)

target_include_directories(osw_proto PUBLIC
    "${OSW_PROTO_GEN_DIR}"
)

target_link_libraries(osw_proto PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# Generated code links into a SHARED .so; position-independent code must
# be on (already set globally, but keep an explicit declaration here so
# the dependency is obvious).
set_target_properties(osw_proto PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# --- buf-lint convenience target (non-blocking when buf is absent) ----
find_program(_osw_buf_bin buf)
if(_osw_buf_bin)
    add_custom_target(osw_proto_lint
        COMMAND "${_osw_buf_bin}" lint
        WORKING_DIRECTORY "${OSW_PROTO_DIR}"
        COMMENT "buf lint (proto/)"
        VERBATIM
    )
    message(STATUS "  buf lint target:   osw_proto_lint (buf at ${_osw_buf_bin})")
else()
    add_custom_target(osw_proto_lint
        COMMAND ${CMAKE_COMMAND} -E echo
                "buf not on PATH — skipping proto lint (CI handles it via bufbuild/buf-action)"
        COMMENT "buf lint (skipped; buf not installed)"
        VERBATIM
    )
    message(STATUS "  buf lint:          buf not found on PATH; osw_proto_lint is a no-op")
endif()
