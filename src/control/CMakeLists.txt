# src/control/CMakeLists.txt — gRPC control plane skeleton.
#
# W1 ships:
#   osw_control      — FS-agnostic. GrpcServer + ControlServiceSkeleton +
#                      Health handler (real impl) + UNIMPLEMENTED stubs
#                      for the rest. Links osw_proto (which carries gRPC
#                      + protobuf transitively).
#
# osw_control_fs is reserved for W3 control handlers that call FS APIs
# (switch_ivr_originate, etc.). W1 doesn't need it; we don't create the
# target so the source-list isn't lying about non-existent files.

add_library(osw_control STATIC
    server.cc
    tls.cc
    handlers/health_handler.cc
    handlers/unimplemented.cc
)
target_include_directories(osw_control
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}                  # for "src/control/..." includes
        ${OSW_PROTO_GEN_DIR}                 # for "open_switch/.../*.pb.h"
)
target_compile_features(osw_control PUBLIC cxx_std_20)
target_link_libraries(osw_control PUBLIC
    osw_observability
    osw_proto
    Threads::Threads
)
set_target_properties(osw_control PROPERTIES POSITION_INDEPENDENT_CODE ON)
