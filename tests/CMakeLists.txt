include(GoogleTest)

add_executable(talus_unit_tests
  unit/arena_test.cc
  unit/block_test.cc
  unit/buffer_pool_test.cc
  unit/coding_test.cc
  unit/crc32c_test.cc
  unit/dbformat_test.cc
  unit/filter_test.cc
  unit/group_commit_test.cc
  unit/hash_test.cc
  unit/log_replay_test.cc
  unit/log_test.cc
  unit/lru_cache_test.cc
  unit/memtable_test.cc
  unit/merging_iterator_test.cc
  unit/skiplist_test.cc
  unit/status_test.cc
  unit/table_cache_test.cc
  unit/table_test.cc
  unit/write_batch_test.cc
)
target_include_directories(talus_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(talus_unit_tests PRIVATE talus talus_warnings GTest::gtest_main)
gtest_discover_tests(talus_unit_tests DISCOVERY_TIMEOUT 60)
