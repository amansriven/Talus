include(FetchContent)

if(Talus_WITH_LZ4)
  # Only lz4.c is needed. SOURCE_SUBDIR names a directory with no
  # CMakeLists.txt, so MakeAvailable downloads LZ4 without configuring its
  # own build.
  FetchContent_Declare(
    lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    SOURCE_SUBDIR talus-no-cmake
  )
  FetchContent_MakeAvailable(lz4)
  add_library(talus_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
  target_include_directories(talus_lz4 SYSTEM PUBLIC ${lz4_SOURCE_DIR}/lib)
endif()

if(Talus_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(Talus_BUILD_BENCH)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    hdrhistogram
    URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.8.tar.gz
  )
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_LOG_REQUIRED OFF CACHE STRING "" FORCE)  # The log format needs zlib; unused.
  FetchContent_MakeAvailable(googlebenchmark hdrhistogram)
endif()

if(Talus_BENCH_COMPARE)
  # Comparison engines for the benchmarks only (never linked into Talus).
  FetchContent_Declare(
    leveldb
    URL https://github.com/google/leveldb/archive/refs/tags/1.23.tar.gz
  )
  set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    rocksdb
    URL https://github.com/facebook/rocksdb/archive/refs/tags/v9.10.0.tar.gz
  )
  foreach(opt WITH_TESTS WITH_ALL_TESTS WITH_BENCHMARK_TOOLS WITH_TOOLS WITH_CORE_TOOLS
              WITH_TRACE_TOOLS WITH_GFLAGS WITH_SNAPPY WITH_LZ4 WITH_ZLIB WITH_ZSTD WITH_BZ2
              WITH_JNI FAIL_ON_WARNINGS ROCKSDB_BUILD_SHARED WITH_LIBURING)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_MakeAvailable(leveldb rocksdb)
endif()
