find_package(GTest CONFIG REQUIRED)

# Unit tests
file(GLOB_RECURSE UNIT_TEST_FILES unit/*.cpp)
add_executable(unit_tests ${UNIT_TEST_FILES})
target_link_libraries(unit_tests PRIVATE GTest::gtest_main)

# Register with CTest so `ctest` picks them up
add_test(NAME unit_tests COMMAND unit_tests)
