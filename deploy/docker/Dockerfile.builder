# open-switch — builder stage
#
# Layers the module on top of simplefs/open-switch-base which already
# bundles FreeSWITCH headers + gRPC C++ + protobuf + abseil + libuuid
# pre-installed at /opt/grpc. CI iterations skip the ~14-min gRPC
# compile this way (the base image is rebuilt once when gRPC version
# changes, via Dockerfile.base; published from native amd64 + arm64
# hosts as a multi-arch manifest).
#
# Base image: docker.io/simplefs/open-switch-base:1.10.12-trixie
#   - multi-arch (linux/amd64 + linux/arm64)
#   - public on Docker Hub (no auth needed for CI pulls)
#
# Build:
#   docker buildx build --platform linux/amd64,linux/arm64 \
#     --build-arg BASE_TAG=1.10.12-trixie \
#     --target fs-builder \
#     -t open-switch/builder:0.1.0 \
#     -f deploy/docker/Dockerfile.builder .

ARG BASE_IMAGE=docker.io/simplefs/open-switch-base
ARG BASE_TAG=1.10.12-trixie
FROM ${BASE_IMAGE}:${BASE_TAG} AS fs-builder

# clang-tidy/clang-tools come from the trixie distro repos; we don't
# need a specific clang version — clang-tidy is a static-analysis tool
# that reads compile_commands.json (produced by our gcc build). The base
# image only ships gcc/g++ to keep size down; this layer adds the
# analyzer for the static-analysis CI job. ~50MB.
#
# When the base image is refreshed (Dockerfile.base), move this apt
# install into the base so derived builds skip the package fetch.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-tidy clang-tools && \
    rm -rf /var/lib/apt/lists/*

# Module source — copy into builder.
WORKDIR /usr/src/open-switch
COPY CMakeLists.txt ./
COPY proto/ ./proto/
COPY src/ ./src/
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY tests/ ./tests/

ARG OSW_ENABLE_ASAN=OFF
ARG OSW_ENABLE_TSAN=OFF
ARG OSW_STRICT_WARNINGS=ON
ARG OSW_BUILD_TESTS=OFF
ARG BUILD_TYPE=Release

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
          -DOSW_ENABLE_ASAN=${OSW_ENABLE_ASAN} \
          -DOSW_ENABLE_TSAN=${OSW_ENABLE_TSAN} \
          -DOSW_STRICT_WARNINGS=${OSW_STRICT_WARNINGS} \
          -DOSW_BUILD_TESTS=${OSW_BUILD_TESTS} \
          -DOSW_FREESWITCH_INCLUDE_DIR=/usr/local/include \
          -DOSW_FREESWITCH_MOD_DIR=/usr/local/mod \
          .. && \
    cmake --build . -j$(nproc) && \
    cmake --install .

# Stage output to /dist.
RUN mkdir -p /dist/lib /dist/mod && \
    cp /usr/local/mod/mod_open_switch.so /dist/mod/ && \
    cp -a ${GRPC_INSTALL_DIR}/lib/lib*.so* /dist/lib/ 2>/dev/null || true

# Sanity: dynamic deps all resolve.
RUN ls -la /dist/mod && file /dist/mod/mod_open_switch.so && \
    LD_LIBRARY_PATH=/dist/lib:/opt/grpc/lib \
      ldd /dist/mod/mod_open_switch.so && \
    ! (LD_LIBRARY_PATH=/dist/lib:/opt/grpc/lib \
       ldd /dist/mod/mod_open_switch.so | grep -E "not found")
