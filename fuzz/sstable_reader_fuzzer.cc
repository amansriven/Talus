// Fuzzes the SSTable reader: arbitrary bytes as a table file, in pread or
// mmap mode (chosen by the input). Opening, iterating and point lookups must
// fail cleanly, never crash.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "table/table.h"
#include "util/file.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace talus;
  static const std::string path =
      (std::filesystem::temp_directory_path() / ("talus_fuzz_" + std::to_string(getpid()) + ".sst"))
          .string();
  {
    std::unique_ptr<WritableFile> file;
    if (!WritableFile::Create(path, &file).ok() ||
        !file->Append(std::string_view(reinterpret_cast<const char*>(data), size)).ok() ||
        !file->Close().ok()) {
      return 0;
    }
  }
  const auto mode =
      size > 0 && (data[0] & 1) != 0 ? RandomAccessFile::Mode::kMmap : RandomAccessFile::Mode::kPread;
  std::unique_ptr<RandomAccessFile> file;
  if (!RandomAccessFile::Open(path, mode, &file).ok()) {
    return 0;
  }
  std::unique_ptr<Table> table;
  if (!Table::Open(TableOpenOptions{}, std::move(file), &table).ok()) {
    return 0;
  }
  auto iter = table->NewIterator();
  size_t steps = 0;
  std::string last_key;
  for (iter->SeekToFirst(); iter->Valid() && steps < 10000; iter->Next(), ++steps) {
    last_key.assign(iter->key());
    volatile size_t sink = iter->value().size();
    (void)sink;
  }
  if (!last_key.empty()) {
    iter->Seek(last_key);
    (void)table->InternalGet(last_key, TableReadOptions{},
                             [](std::string_view, std::string_view, Pin&) {});
  }
  return 0;
}
