# open-switch — mod_open_switch.so build
#
# Builds a FreeSWITCH module that bridges FS to external services via gRPC.
# Events are delivered via gRPC SubscribeEvents (in-memory per-tier rings,
# broadcast to subscribers). There is no in-module Redis / Kafka / NATS
# producer; subscribers persist as they choose. See
# openspec/changes/core-module-v1/designs/transport-adr.md.
#
# Required system deps (provided by the builder image, see deploy/docker/Dockerfile.builder):
#   - FreeSWITCH headers (linked from /usr/local/include/freeswitch by default)
#   - gRPC C++ (latest stable, system or vendored)
#   - Protobuf
#   - Abseil (gRPC dep)
#   - OpenSSL
#   - libuuid
#
# Build (in builder container):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   cmake --install build  # places mod_open_switch.so into ${FS_MOD_DIR}
#
# Optional flags:
#   -DOSW_BUILD_TESTS=ON     enable unit tests (gtest)
#   -DOSW_ENABLE_ASAN=ON     AddressSanitizer + LeakSanitizer (CI default)
#   -DOSW_ENABLE_TSAN=ON     ThreadSanitizer (mutually exclusive with ASAN)
#   -DOSW_STRICT_WARNINGS=ON treat warnings as errors (CI default)

cmake_minimum_required(VERSION 3.22)

project(mod_open_switch
    VERSION 0.1.0
    DESCRIPTION "FreeSWITCH gRPC + event bridge module"
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# ─── Options ───────────────────────────────────────────────────────────
option(OSW_BUILD_TESTS      "Build unit + integration tests"      OFF)
option(OSW_ENABLE_ASAN      "AddressSanitizer + LeakSanitizer"    OFF)
option(OSW_ENABLE_TSAN      "ThreadSanitizer"                     OFF)
option(OSW_ENABLE_UBSAN     "UndefinedBehaviorSanitizer"          OFF)
option(OSW_STRICT_WARNINGS  "Treat compiler warnings as errors"   OFF)
option(OSW_INSTALL_MODULE   "Install .so to FreeSWITCH mod dir"   ON)

set(OSW_FREESWITCH_INCLUDE_DIR "/usr/local/include/freeswitch"
    CACHE PATH "Location of FreeSWITCH headers")
set(OSW_FREESWITCH_MOD_DIR "/usr/local/mod"
    CACHE PATH "Where to install mod_open_switch.so")

if(OSW_ENABLE_ASAN AND OSW_ENABLE_TSAN)
    message(FATAL_ERROR "OSW_ENABLE_ASAN and OSW_ENABLE_TSAN are mutually exclusive")
endif()

# ─── Compiler flags ────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Isolate statically-linked deps inside our .so (gRPC + protobuf + abseil
# all link static). Prevents symbol collision with other FreeSWITCH modules
# that may link a different gRPC version. The module's SWITCH_MOD_DECLARE
# export macros remain visible via __attribute__((visibility("default"))).
# (Phase 1 Codex finding N-5: ensure -Wl,--exclude-libs,ALL is set.)
add_link_options(-Wl,--exclude-libs,ALL)

if(OSW_STRICT_WARNINGS)
    add_compile_options(-Werror)
endif()

if(OSW_ENABLE_ASAN)
    message(STATUS "AddressSanitizer + LeakSanitizer enabled")
    add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,leak)
endif()

if(OSW_ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

if(OSW_ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# ─── Dependencies ──────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# gRPC + Protobuf (prefer system install)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION} / Protobuf ${Protobuf_VERSION}")

# UUID
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid)

# FreeSWITCH headers (header-only — no link, the .so is loaded into FS process)
if(NOT EXISTS "${OSW_FREESWITCH_INCLUDE_DIR}/switch.h")
    message(FATAL_ERROR "FreeSWITCH headers not found at ${OSW_FREESWITCH_INCLUDE_DIR}")
endif()

# ─── Generated proto sources ──────────────────────────────────────────
set(OSW_PROTO_DIR     "${CMAKE_SOURCE_DIR}/proto")
set(OSW_PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/proto-gen")
file(MAKE_DIRECTORY "${OSW_PROTO_GEN_DIR}")

# Wires up protoc + grpc_cpp_plugin to emit
# ${OSW_PROTO_GEN_DIR}/open_switch/{control,events,media}/v1/*.{pb,grpc.pb}.{h,cc}.
# Produces the internal static library `osw_proto`, plus the
# `osw_proto_lint` convenience target (non-blocking when `buf` is absent).
# Generated sources are excluded from clang-tidy via .clang-tidy's
# HeaderFilterRegex; they live entirely under build/proto-gen.
include("${CMAKE_SOURCE_DIR}/cmake/proto.cmake")

# ─── Module target ──────────────────────────────────────────────────────
# W1 Foundation (this wave): src/ holds the real module shell —
# `osw::Module` singleton (core/), `osw::log` + `osw::Health`
# (observability/), `osw::control::GrpcServer` skeleton with Health
# RPC + UNIMPLEMENTED handlers for the rest (control/), and the
# header-only RAII helpers under include/osw/raii/. Event plane,
# control RPC implementations beyond Health, and media bug manager
# land in subsequent waves (W2, W3, W4).
# See openspec/changes/core-module-v1/implementation/W1-foundation.md.
add_subdirectory(src)

# ─── Tests ────────────────────────────────────────────────────────────
if(OSW_BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)
endif()

# ─── Summary ──────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "open-switch configuration:")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "  FS include dir:    ${OSW_FREESWITCH_INCLUDE_DIR}")
message(STATUS "  FS mod install dir: ${OSW_FREESWITCH_MOD_DIR}")
message(STATUS "  Build tests:       ${OSW_BUILD_TESTS}")
message(STATUS "  ASAN:              ${OSW_ENABLE_ASAN}")
message(STATUS "  TSAN:              ${OSW_ENABLE_TSAN}")
message(STATUS "  UBSAN:             ${OSW_ENABLE_UBSAN}")
message(STATUS "  Strict warnings:   ${OSW_STRICT_WARNINGS}")
message(STATUS "")
