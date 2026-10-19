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
target_link_libraries(micro_bench PRIVATE talus benchmark::benchmark talus_warnings)
