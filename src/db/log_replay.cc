#include "db/log_replay.h"

#include <algorithm>
#include <memory>
#include <string_view>

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "util/file.h"
#include "wal/log_reader.h"

namespace talus {

Status ReplayLogFile(const std::string& path, bool paranoid_checks, MemTable* mem,
                     SequenceNumber* max_sequence) {
  std::unique_ptr<SequentialFile> file;
  Status s = SequentialFile::Open(path, &file);
  if (!s.ok()) {
    return s;
  }
  log::Reader reader(std::move(file));
  std::string scratch;
  std::string_view record;
  while (true) {
    switch (reader.ReadRecord(&record, &scratch)) {
      case log::Reader::Result::kEof:
        return Status::OK();
      case log::Reader::Result::kIOError:
        return reader.error();
      case log::Reader::Result::kCorruption:
        return paranoid_checks ? Status::Corruption(path, reader.error().message()) : Status::OK();
      case log::Reader::Result::kRecord:
        break;
    }
    s = WriteBatchInternal::Validate(record);
    if (!s.ok()) {
      return paranoid_checks ? Status::Corruption(path, s.message()) : Status::OK();
    }
    s = WriteBatchInternal::InsertInto(record, mem);
    if (!s.ok()) {
      return s;  // Unreachable after Validate().
    }
    const uint32_t count = DecodeFixed32(record.data() + 8);
    if (count > 0) {
      *max_sequence = std::max(*max_sequence, DecodeFixed64(record.data()) + count - 1);
    }
  }
}

}  // namespace talus
