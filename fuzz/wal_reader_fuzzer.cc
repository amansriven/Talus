// Fuzzes WAL recovery: arbitrary bytes as a log file, replayed into a
// memtable, in both normal and paranoid mode. This covers the record reader,
// WriteBatch decoding and validation, and memtable insertion. Replay must
// return OK or Corruption and never crash.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "db/log_replay.h"
#include "memtable/memtable.h"
#include "util/file.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace talus;
  static const std::string path =
      (std::filesystem::temp_directory_path() / ("talus_fuzz_" + std::to_string(getpid()) + ".log"))
          .string();
  {
    std::unique_ptr<WritableFile> file;
    if (!WritableFile::Create(path, &file).ok() ||
        !file->Append(std::string_view(reinterpret_cast<const char*>(data), size)).ok() ||
        !file->Close().ok()) {
      return 0;
    }
  }
  for (bool paranoid : {false, true}) {
    MemTable mem;
    SequenceNumber max_sequence = 0;
    const Status s = ReplayLogFile(path, paranoid, &mem, &max_sequence);
    if (!s.ok() && !s.IsCorruption()) {
      std::abort();  // Only Corruption is an acceptable failure for bad bytes.
    }
    if (!paranoid && !s.ok()) {
      std::abort();  // Non-paranoid replay stops at damage instead of failing.
    }
  }
  return 0;
}
