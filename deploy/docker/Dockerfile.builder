# open-switch — builder stage
#
# Extends the open-gateway FreeSWITCH builder with the deps mod_open_switch
# needs: gRPC C++ + protobuf + abseil + libuuid.
# Outputs a /dist directory containing mod_open_switch.so ready to be
# layered into the runner image.
#
# Phase-1-fix-sprint note: the previous draft of this Dockerfile installed
# hiredis + redis-plus-plus for an in-module Redis event sink. That sink
# was removed (see openspec/changes/core-module-v1/designs/transport-adr.md
# revision). The module now ships zero in-process event-bus producers and
# delivers events via gRPC streaming only. The Redis deps are removed.
#
# Requires the upstream builder image to be built and tagged. See
# https://github.com/luongdev/open-gateway/tree/main/docker/freeswitch/builder
#
# Build:
#   docker buildx build --platform linux/amd64,linux/arm64 \
#     --build-arg UPSTREAM_TAG=v1.10.12 \
#     -t open-switch/builder:0.1.0 \
#     -f deploy/docker/Dockerfile.builder .

ARG UPSTREAM_TAG=v1.10.12
FROM open-gateway/freeswitch-builder:${UPSTREAM_TAG} AS fs-builder

# ─── 1. Install gRPC C++ build deps ──────────────────────────────────
ARG GRPC_VERSION=v1.74.0

RUN apt-get update -yq && \
    apt-get install -yq --no-install-recommends \
        ca-certificates curl git \
        # gRPC build deps
        autoconf automake libtool pkg-config \
        # libuuid for UUIDv7 generation
        uuid-dev \
        # ASAN / UBSAN / Valgrind support
        valgrind \
    && rm -rf /var/lib/apt/lists/*

# ─── 2. Build gRPC + Protobuf + Abseil from source (latest stable) ────
# Note: cannot use Debian-packaged grpc; trixie ships an older version
# missing C++ headers we need. Build from source, install to /opt/grpc.
ENV GRPC_INSTALL_DIR=/opt/grpc

RUN mkdir -p /usr/src && cd /usr/src && \
    git clone --recurse-submodules -b ${GRPC_VERSION} --depth 1 \
        --shallow-submodules https://github.com/grpc/grpc.git grpc && \
    cd grpc && mkdir -p cmake/build && cd cmake/build && \
    cmake -DgRPC_INSTALL=ON \
          -DgRPC_BUILD_TESTS=OFF \
          -DgRPC_BUILD_CSHARP_EXT=OFF \
          -DCMAKE_INSTALL_PREFIX=${GRPC_INSTALL_DIR} \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          ../.. && \
    make -j$(nproc) && make install && \
    cd / && rm -rf /usr/src/grpc

ENV PATH=${GRPC_INSTALL_DIR}/bin:${PATH}
ENV PKG_CONFIG_PATH=${GRPC_INSTALL_DIR}/lib/pkgconfig
ENV CMAKE_PREFIX_PATH=${GRPC_INSTALL_DIR}
ENV LD_LIBRARY_PATH=${GRPC_INSTALL_DIR}/lib:${LD_LIBRARY_PATH}

# ─── 3. Copy module source + build ────────────────────────────────────
WORKDIR /usr/src/open-switch
COPY CMakeLists.txt ./
COPY proto/ ./proto/
COPY src/ ./src/
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY tests/ ./tests/

ARG OSW_ENABLE_ASAN=OFF
ARG OSW_STRICT_WARNINGS=ON
ARG OSW_BUILD_TESTS=OFF
ARG BUILD_TYPE=Release

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
          -DOSW_ENABLE_ASAN=${OSW_ENABLE_ASAN} \
          -DOSW_STRICT_WARNINGS=${OSW_STRICT_WARNINGS} \
          -DOSW_BUILD_TESTS=${OSW_BUILD_TESTS} \
          -DOSW_FREESWITCH_INCLUDE_DIR=/usr/local/include/freeswitch \
          -DOSW_FREESWITCH_MOD_DIR=/usr/local/mod \
          .. && \
    cmake --build . -j$(nproc) && \
    cmake --install .

# ─── 4. Stage output to /dist ─────────────────────────────────────────
RUN mkdir -p /dist/lib /dist/mod && \
    cp /usr/local/mod/mod_open_switch.so /dist/mod/ && \
    cp -a ${GRPC_INSTALL_DIR}/lib/lib*.so* /dist/lib/ 2>/dev/null || true

# Sanity: show what we built + verify all dynamic deps resolve.
RUN ls -la /dist/mod && file /dist/mod/mod_open_switch.so && \
    LD_LIBRARY_PATH=/dist/lib:/opt/grpc/lib \
      ldd /dist/mod/mod_open_switch.so && \
    LD_LIBRARY_PATH=/dist/lib:/opt/grpc/lib \
      ldd /dist/mod/mod_open_switch.so | grep -E "not found" && \
      { echo "FATAL: missing shared deps"; exit 1; } || true
