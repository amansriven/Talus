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
