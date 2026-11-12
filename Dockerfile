# syntax=docker/dockerfile:1
#
# One image containing both binaries: miniLB (default command) and
# demo_backend (used by docker-compose for the three demo backends).

# ---- Stage 1: build ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates cmake g++ make git \
      libboost-dev libhiredis-dev libyaml-cpp-dev libspdlog-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY backends/ backends/

# Tests are not needed inside the image. redis-plus-plus is fetched from
# GitHub by CMake during this step.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMINILB_BUILD_TESTS=OFF \
 && cmake --build build -j "$(nproc)"

# ---- Stage 2: runtime ---------------------------------------------------------
# Only the shared libraries the binaries need: no compiler, no sources.
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libhiredis1.1.0 libyaml-cpp0.8 libspdlog1.12 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --no-create-home minilb

COPY --from=build /src/build/miniLB /src/build/demo_backend /usr/local/bin/
COPY config/config.docker.yaml /etc/minilb/config.yaml

# Do not run as root.
USER minilb
EXPOSE 8080

ENTRYPOINT ["miniLB"]
CMD ["--config", "/etc/minilb/config.yaml"]
