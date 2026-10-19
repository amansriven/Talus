add_library(talus
  db/dbformat.cc
  memtable/memtable.cc
  util/arena.cc
  util/coding.cc
  util/crc32c.cc
  util/crc32c_arm64.cc
  util/crc32c_x86.cc
  util/status.cc
)

target_include_directories(talus
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(talus PUBLIC Threads::Threads PRIVATE talus_warnings)

# The x86 SSE4.2 path uses a function-level target attribute and needs no
# flags. On Linux ARM64 the CRC extension is optional, so that one file is
# compiled with it enabled and the code checks AT_HWCAP before using it. Every
# Apple ARM64 target enables it by default.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set_source_files_properties(util/crc32c_arm64.cc PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
endif()
