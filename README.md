# RedisLite

Lightweight Redis implementation in C++23. Built from scratch to understand
the Redis protocol, event loops, and network programming.

## Build

Requires CMake 3.13+, a C++23 compiler, and vcpkg.

```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

## Run

```sh
./build/redis-lite
```

Listens on port 6379 (default Redis port).
