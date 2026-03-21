# === Build stage ===
FROM gcc:14 AS builder

RUN apt-get update && apt-get install -y cmake git curl zip unzip tar pkg-config

# vcpkg for dependency management (currently only gtest)
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg && /opt/vcpkg/bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /app

# Copy dependency manifests first — Docker layer cache means
# vcpkg install only reruns when dependencies change
COPY vcpkg.json vcpkg-configuration.json ./
COPY CMakeLists.txt ./
COPY tests/CMakeLists.txt tests/

COPY src/ src/
COPY tests/unit/ tests/unit/

RUN cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
RUN cmake --build build

# === Runtime stage ===
# gcc:14 is based on Debian Trixie — must match so glibc/libstdc++ versions align
FROM debian:trixie-slim

# Only runtime dep — libstdc++ for C++ standard library
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/build/redis-lite /usr/local/bin/redis-lite

EXPOSE 6379
ENTRYPOINT ["redis-lite"]
