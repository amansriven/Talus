# Benchmarks (spec §12): db_bench, ycsb and the Google Benchmark
# microbenchmarks. LevelDB and RocksDB are built only with
# -DTalus_BENCH_COMPARE=ON.

add_library(talus_bench_common STATIC bench_common.cc engine.cc perf_counters.cc)
target_include_directories(talus_bench_common
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(talus_bench_common PUBLIC talus hdr_histogram_static PRIVATE talus_warnings)
if(Talus_BENCH_COMPARE)
  target_link_libraries(talus_bench_common PRIVATE leveldb rocksdb)
  target_compile_definitions(talus_bench_common PRIVATE TALUS_BENCH_COMPARE=1)
endif()

foreach(tool db_bench ycsb)
  add_executable(${tool} ${tool}.cc)
  target_link_libraries(${tool} PRIVATE talus_bench_common talus_warnings)
endforeach()

add_executable(micro_bench micro/micro_bench.cc)
target_include_directories(micro_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(micro_bench PRIVATE talus_bench_common benchmark::benchmark talus_warnings)

if(Talus_BUILD_TESTS)
  # Needed here too: before CMake 3.29 the discovery script path is a
  # directory-scoped variable, so tests/'s include does not reach bench/.
  include(GoogleTest)
  add_executable(talus_bench_timer_test timer_test.cc)
  target_link_libraries(talus_bench_timer_test PRIVATE talus_warnings GTest::gtest_main)
  gtest_discover_tests(talus_bench_timer_test DISCOVERY_TIMEOUT 60)

  add_executable(talus_bench_harness_test harness_test.cc)
  target_link_libraries(talus_bench_harness_test PRIVATE talus_bench_common talus_warnings GTest::gtest_main)
  gtest_discover_tests(talus_bench_harness_test DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    if(Talus_BENCH_COMPARE)
      set(_smoke_engines talus,leveldb,rocksdb)
    else()
      set(_smoke_engines talus)
    endif()
    add_test(NAME BenchmarkSmoke COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke.py --build ${PROJECT_BINARY_DIR}
      --out ${PROJECT_BINARY_DIR}/benchmark-smoke --engines ${_smoke_engines})
    set_tests_properties(BenchmarkSmoke PROPERTIES TIMEOUT 600)
  endif()
endif()
