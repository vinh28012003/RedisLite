find_package(GTest CONFIG REQUIRED)

# Source files to test (exclude main.cpp - it has main())
set(LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/resp_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/command.cpp
    ${CMAKE_SOURCE_DIR}/src/store.cpp
    ${CMAKE_SOURCE_DIR}/src/server.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)

# Unit tests
file(GLOB_RECURSE UNIT_TEST_FILES unit/*.cpp)
add_executable(unit_tests ${UNIT_TEST_FILES} ${LIB_SOURCES})
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_tests PRIVATE GTest::gtest_main)

# Register with CTest so `ctest` picks them up
include(GoogleTest)                                                                                     
gtest_discover_tests(unit_tests)    


