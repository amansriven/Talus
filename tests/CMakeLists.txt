include(GoogleTest)

add_executable(talus_unit_tests
  unit/arena_test.cc
  unit/coding_test.cc
  unit/crc32c_test.cc
  unit/hash_test.cc
  unit/status_test.cc
)
target_include_directories(talus_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(talus_unit_tests PRIVATE talus talus_warnings GTest::gtest_main)
gtest_discover_tests(talus_unit_tests DISCOVERY_TIMEOUT 60)
