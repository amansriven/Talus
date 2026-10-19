#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Talus/status.h"
#include "Talus/write_batch.h"
#include "db/dbformat.h"
#include "util/coding.h"

namespace talus {

class MemTable;

// Operations on WriteBatch that are not part of the public API.
//
// Serialized format:
//   sequence (fixed64) | count (fixed32) | op[count]
//   op := kValue    (1B = 0x01) | varint32 key_len | key | varint32 value_len | value
//       | kDeletion (1B = 0x00) | varint32 key_len | key
class WriteBatchInternal {
 public:
  static constexpr size_t kHeaderSize = 12;

  static SequenceNumber Sequence(const WriteBatch& b) { return DecodeFixed64(b.rep_.data()); }
  static void SetSequence(WriteBatch* b, SequenceNumber seq) { EncodeFixed64(b->rep_.data(), seq); }
  static uint32_t Count(const WriteBatch& b) { return DecodeFixed32(b.rep_.data() + 8); }
  static void SetCount(WriteBatch* b, uint32_t n) { EncodeFixed32(b->rep_.data() + 8, n); }

  static std::string_view Contents(const WriteBatch& b) { return b.rep_; }
  static size_t ByteSize(const WriteBatch& b) { return b.rep_.size(); }

  // Replaces the batch with a serialized one (e.g. a WAL record). Only the
  // header length is checked here; Iterate() validates the operations.
  static Status SetContents(WriteBatch* b, std::string_view contents);

  // Decodes serialized batch `contents`, calling handler.Put(key, value) or
  // handler.Delete(key) for each operation in order. Returns Corruption if
  // the contents are malformed or the operation count is wrong. Handlers may
  // already have been called for earlier operations when that happens.
  template <typename Handler>
  static Status Iterate(std::string_view contents, Handler&& handler);

  // Checks that `contents` is a well-formed batch without applying it.
  static Status Validate(std::string_view contents);

  // Adds the batch's operations to `mem`, numbered consecutively from the
  // batch's sequence number.
  static Status InsertInto(std::string_view contents, MemTable* mem);
};

template <typename Handler>
Status WriteBatchInternal::Iterate(std::string_view contents, Handler&& handler) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  const uint32_t expected = DecodeFixed32(contents.data() + 8);
  std::string_view input = contents.substr(kHeaderSize);
  uint32_t found = 0;
  while (!input.empty()) {
    const auto tag = static_cast<ValueType>(input.front());
    input.remove_prefix(1);
    std::string_view key;
    std::string_view value;
    switch (tag) {
      case ValueType::kValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("bad WriteBatch Put");
        }
        handler.Put(key, value);
        break;
      case ValueType::kDeletion:
        if (!GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
        handler.Delete(key);
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
    ++found;
  }
  if (found != expected) {
    return Status::Corruption("WriteBatch has wrong count");
  }
  return Status::OK();
}

}  // namespace talus
