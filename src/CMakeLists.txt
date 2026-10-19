add_library(talus
  cache/table_cache.cc
  db/dbformat.cc
  db/group_commit.cc
  db/log_replay.cc
  db/write_batch.cc
  db/write_thread.cc
  memtable/memtable.cc
  table/block.cc
  table/block_builder.cc
  table/filter_policy.cc
  table/format.cc
  table/table.cc
  table/table_builder.cc
  util/arena.cc
  util/buffer_pool.cc
  util/coding.cc
  util/crc32c.cc
  util/crc32c_arm64.cc
  util/crc32c_x86.cc
  util/file.cc
  util/filename.cc
  util/status.cc
  wal/log_reader.cc
  wal/log_writer.cc
)

target_include_directories(talus
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(talus PUBLIC Threads::Threads PRIVATE talus_warnings)

if(Talus_WITH_LZ4)
  target_link_libraries(talus PRIVATE talus_lz4)
  target_compile_definitions(talus PRIVATE TALUS_HAVE_LZ4=1)
endif()

# The x86 SSE4.2 path uses a function-level target attribute and needs no
# flags. On Linux ARM64 the CRC extension is optional, so that one file is
# compiled with it enabled and the code checks AT_HWCAP before using it. Every
# Apple ARM64 target enables it by default.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set_source_files_properties(util/crc32c_arm64.cc PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
endif()
