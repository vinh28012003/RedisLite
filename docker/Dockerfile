FROM gcc:14 as builder

# Install cmake and vcpkg dependencies
RUN apt-get update && apt-get install -y cmake git curl zip unzip tar pkg-config

# Install vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg && /opt/vcpkg/bootstrap-vcpkg.sh

ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /app
# Copy dependency manifests first - Docker caches this layer
COPY vcpkg.json vcpkg-configuration.json ./
COPY CMakeLists.txt ./
COPY tests/CMakeLists.txt tests/

# Copy source
COPY src/ src/
COPY tests/unit/ tests/unit/

# Build 
RUN cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
RUN cmake --build build

EXPOSE 6379
CMD ["./build/redis-lite"]


