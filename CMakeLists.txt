cmake_minimum_required(VERSION 3.20)

project(Talus VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # Timestamp extracted FetchContent archives.
endif()

option(Talus_BUILD_TESTS "Build unit and integration tests" ON)
option(Talus_WITH_LZ4 "Enable optional LZ4 block compression" OFF)
option(Talus_BENCH_COMPARE "Build LevelDB and RocksDB for comparison benchmarks" OFF)
set(Talus_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address;undefined' or 'thread'")

# Sanitizers apply to every target, dependencies included, so that TSan sees
# all synchronization and ASan sees all allocations.
if(Talus_SANITIZE)
  list(JOIN Talus_SANITIZE "," _talus_sanitizers)
  add_compile_options(-fsanitize=${_talus_sanitizers} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_talus_sanitizers})
endif()

# Strict warnings apply only to Talus's own targets, not to fetched dependencies.
add_library(talus_warnings INTERFACE)
target_compile_options(talus_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)
include(cmake/Dependencies.cmake)

add_subdirectory(src)

if(Talus_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
