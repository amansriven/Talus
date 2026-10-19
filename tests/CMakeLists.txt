include(GoogleTest)

add_executable(talus_unit_tests
  unit/arena_test.cc
  unit/block_test.cc
  unit/buffer_pool_test.cc
  unit/coding_test.cc
  unit/crc32c_test.cc
  unit/db_test.cc
  unit/dbformat_test.cc
  unit/filter_test.cc
  unit/group_commit_test.cc
  unit/hash_test.cc
  unit/log_replay_test.cc
  unit/log_test.cc
  unit/lru_cache_test.cc
  unit/memtable_test.cc
  unit/merging_iterator_test.cc
  unit/rate_limiter_test.cc
  unit/skiplist_test.cc
  unit/status_test.cc
  unit/table_cache_test.cc
  unit/table_test.cc
  unit/version_edit_test.cc
  unit/version_set_test.cc
  unit/write_batch_test.cc
)
target_include_directories(talus_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(talus_unit_tests PRIVATE talus talus_warnings GTest::gtest_main)
gtest_discover_tests(talus_unit_tests DISCOVERY_TIMEOUT 60)

# Model-based randomized test (spec §11): 1M seeded operations against a
# std::map model by default. A fixed seed keeps CI runs reproducible.
# Sanitized builds run 10-30x slower, so they use fewer operations.
if(Talus_SANITIZE)
  set(Talus_MODEL_OPS 150000 CACHE STRING "Operations in the ctest model test")
else()
  set(Talus_MODEL_OPS 1000000 CACHE STRING "Operations in the ctest model test")
endif()
add_executable(talus_model_test model/model_test.cc)
target_link_libraries(talus_model_test PRIVATE talus talus_warnings)
add_test(NAME ModelTest COMMAND talus_model_test --seed 301 --ops ${Talus_MODEL_OPS})
set_tests_properties(ModelTest PROPERTIES TIMEOUT 3600)

# The failure path: an injected mismatch must fail and write a log, and
# replaying that log must fail again at the same op.
add_test(NAME ModelTestInjectedFailure
         COMMAND talus_model_test --seed 7 --ops 5000 --inject-mismatch-at 3000)
set_tests_properties(ModelTestInjectedFailure PROPERTIES
                     WILL_FAIL TRUE FIXTURES_SETUP model_failure_log)
add_test(NAME ModelTestReplayReproduces
         COMMAND talus_model_test --replay talus_model_failure_7.txt --inject-mismatch-at 3000)
set_tests_properties(ModelTestReplayReproduces PROPERTIES
                     WILL_FAIL TRUE FIXTURES_REQUIRED model_failure_log)

# Each binary below covers one item of spec §11.
function(talus_gtest name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/unit)
  target_link_libraries(${name} PRIVATE talus talus_warnings GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 1800)
endfunction()

talus_gtest(talus_concurrent_model_test model/concurrent_model_test.cc)
talus_gtest(talus_corruption_test corruption/corruption_test.cc)
talus_gtest(talus_stress_test stress/stress_test.cc)
talus_gtest(talus_alloc_test alloc/alloc_test.cc)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND "thread" IN_LIST Talus_SANITIZE)
  # Clang's Linux TSan runtime defines strong global new/delete symbols.
  # Count calls at link time while preserving the runtime's allocator and
  # keeping all allocation assertions active. These are the 64-bit Itanium
  # ABI symbols for scalar/array, aligned and nothrow allocation overloads.
  target_compile_definitions(talus_alloc_test PRIVATE TALUS_ALLOC_WRAP_NEW=1)
  foreach(symbol _Znwm _Znam _ZnwmSt11align_val_t _ZnamSt11align_val_t
                 _ZnwmRKSt9nothrow_t _ZnamRKSt9nothrow_t)
    target_link_options(talus_alloc_test PRIVATE "LINKER:--wrap=${symbol}")
  endforeach()
endif()

# Crash harness: 100 SIGKILL iterations by default (the long run is
# `talus_crash_test --iterations 10000`).
add_executable(talus_crash_test crash/crash_test.cc)
target_link_libraries(talus_crash_test PRIVATE talus talus_warnings)
add_test(NAME CrashTest COMMAND talus_crash_test --iterations 100 --seed 301)
set_tests_properties(CrashTest PROPERTIES TIMEOUT 1800)
