#include "wal/log_reader.h"

#include <utility>

#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/log_format.h"

namespace talus::log {

Reader::Reader(std::unique_ptr<SequentialFile> file) : file_(std::move(file)) {}

Reader::Result Reader::ReadRecord(std::string_view* record, std::string* scratch) {
  char header[kHeaderSize];
  size_t n = 0;
  Status s = file_->Read(kHeaderSize, header, &n);
  if (!s.ok()) {
    error_ = std::move(s);
    return Result::kIOError;
  }
  if (n < kHeaderSize) {
    return Result::kEof;  // Clean end, or a header torn by a crash.
  }
  const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
  const uint32_t length = DecodeFixed32(header + 4);

  // A length reaching past the end of the file means the payload was torn by
  // a crash. Checking against the file size first also stops a corrupt length
  // from causing a huge allocation.
  if (length > file_->size() - file_->position()) {
    return Result::kEof;
  }
  scratch->resize(length);
  s = file_->Read(length, scratch->data(), &n);
  if (!s.ok()) {
    error_ = std::move(s);
    return Result::kIOError;
  }
  if (n < length) {
    return Result::kEof;
  }

  // header[8] is the type byte, which the CRC covers along with the payload.
  const uint32_t actual_crc = crc32c::Extend(crc32c::Value(header + 8, 1), scratch->data(), length);
  if (actual_crc != expected_crc) {
    error_ = Status::Corruption("log record checksum mismatch");
    return Result::kCorruption;
  }
  if (static_cast<uint8_t>(header[8]) != kFullType) {
    error_ = Status::Corruption("unknown log record type");
    return Result::kCorruption;
  }
  *record = *scratch;
  return Result::kRecord;
}

}  // namespace talus::log
