#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Talus/status.h"
#include "db/dbformat.h"

namespace talus {

// Metadata for one SSTable.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // Smallest internal key in the file.
  std::string largest;   // Largest internal key in the file.
};

// A change to the set of live files, plus counters. The MANIFEST is a log of
// these, one per record (WAL record format).
//
// Encoding: a sequence of fields, each a varint32 tag followed by its payload:
//   1 log_number        varint64
//   2 next_file_number  varint64
//   3 last_sequence     varint64
//   4 compact_pointer   varint32 level | length-prefixed internal key
//   5 deleted_file      varint32 level | varint64 number
//   6 new_file          varint32 level | varint64 number | varint64 size
//                       | length-prefixed smallest | length-prefixed largest
class VersionEdit {
 public:
  // Levels are validated against this when decoding; the DB checks its own
  // num_levels on top.
  static constexpr int kMaxLevels = 64;

  void SetLogNumber(uint64_t n) { log_number = n; }
  void SetNextFileNumber(uint64_t n) { next_file_number = n; }
  void SetLastSequence(SequenceNumber s) { last_sequence = s; }
  void SetCompactPointer(int level, std::string_view key) {
    compact_pointers.emplace_back(level, std::string(key));
  }
  void AddFile(int level, const FileMetaData& f) { new_files.emplace_back(level, f); }
  void DeleteFile(int level, uint64_t number) { deleted_files.emplace(level, number); }

  void EncodeTo(std::string* dst) const;
  // Strict: an unknown tag, a truncated field, a level >= kMaxLevels or an
  // internal key shorter than its tag is Corruption.
  Status DecodeFrom(std::string_view src);

  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file_number;
  std::optional<SequenceNumber> last_sequence;
  std::vector<std::pair<int, std::string>> compact_pointers;
  std::set<std::pair<int, uint64_t>> deleted_files;
  std::vector<std::pair<int, FileMetaData>> new_files;
};

}  // namespace talus
