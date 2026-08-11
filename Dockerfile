# SPDX-License-Identifier: Apache-2.0
#
# Multi-stage build for fastcached. The build stage compiles a Release binary
# with TLS enabled (system OpenSSL); the runtime stage is a slim image carrying
# just the binary and the shared libraries it needs.
#
#   docker build -t fastcached .
#   docker run --rm -p 6674:6674 -p 9259:9259 fastcached \
#       --metrics --requirepass=secret
#
# The image binds 0.0.0.0 so it is reachable from outside the container — pair
# that with --requirepass (and --tls for encryption) for any non-trusted network.
#
# Base is ubuntu:26.04: it ships CMake >= 3.28 (the project floor) and a C++23
# g++. yaml-cpp is NOT installed, so CPM fetches and links it statically —
# the runtime image then needs only the OpenSSL runtime (pulled by the `openssl`
# package) on top of the base C/C++ runtime.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:26.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build g++ libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ \
        -DFASTCACHED_BUILD_TESTS=OFF \
        -DFASTCACHED_ENABLE_TLS=ON \
        -DUSE_COMPILER_CACHE=OFF \
    && cmake --build build --target fastcached

# ---- runtime stage --------------------------------------------------------
FROM ubuntu:26.04 AS runtime

# Installing `openssl` pulls in the matching libssl/libcrypto runtime without
# pinning a version-suffixed package name; libstdc++/libgcc are already present
# in the base image. yaml-cpp is linked statically, so no extra package needed.
RUN apt-get update && apt-get install -y --no-install-recommends \
        openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/target/fastcached /usr/local/bin/fastcached

# Cache on 6674 (fastcached's own port), admin/metrics on 9259.
EXPOSE 6674 9259

# Self-probe /healthz using the binary itself (no curl in the image). Requires
# the daemon to be started with --metrics (see CMD).
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD ["fastcached", "--healthcheck"]

ENTRYPOINT ["fastcached"]
# Default: listen on all interfaces with the metrics endpoint enabled. Override
# at `docker run` to add --requirepass / --tls etc. No --port: restating the
# compiled default here is how EXPOSE and the daemon drift apart.
CMD ["--bind=0.0.0.0", "--metrics", "--metrics-bind=0.0.0.0"]
