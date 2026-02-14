cmake_minimum_required(VERSION 3.13)

project(RedisLite)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_STANDARD 23)

add_executable(redis-lite ${SOURCE_FILES})

# Testing
enable_testing()
add_subdirectory(tests)