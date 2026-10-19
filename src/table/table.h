#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "Talus/status.h"
#include "db/dbformat.h"
#include "table/block.h"
#include "table/filter_policy.h"
#include "table/iterator.h"
#include "util/file.h"
#include "util/hash.h"

namespace talus {

// An open, immutable SSTable. Opening parses the footer and keeps the index
// block and the filter in memory. Data blocks are read on demand. Thread-safe.
class Table {
  struct PrivateTag {};

 public:
  static Status Open(std::unique_ptr<RandomAccessFile> file, std::unique_ptr<Table>* table);

  // Use Open(). (Public only so std::make_unique can call it.)
  explicit Table(PrivateTag, std::unique_ptr<RandomAccessFile> file);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // False if the filter proves the table holds no entry for `user_key`.
  bool KeyMayMatch(std::string_view user_key) const {
    return filter_.empty() || FilterMayContain(filter_, Hash64(user_key));
  }

  // Point lookup. Finds the first entry with key >= `internal_key` (normally
  // a LookupKey) and, if there is one, calls fn(key, value) while its block
  // is loaded. The callback checks whether the user key matches. Skips all
  // I/O when the filter rules the user key out.
  template <typename Fn>
  Status InternalGet(std::string_view internal_key, Fn&& fn) const;

  // Iterates every entry in order. The iterator must not outlive the table.
  std::unique_ptr<InternalIterator> NewIterator() const;

 private:
  friend class TableIterator;

  // Decodes a BlockHandle from an index entry's value and reads that block.
  Status ReadDataBlock(std::string_view index_value, Block* block) const;

  std::unique_ptr<RandomAccessFile> file_;
  Block index_block_;
  // The filter, copied into a 64-byte-aligned buffer so each block of a
  // blocked bloom filter is exactly one cache line.
  std::unique_ptr<char[]> filter_storage_;
  std::string_view filter_;
};

template <typename Fn>
Status Table::InternalGet(std::string_view internal_key, Fn&& fn) const {
  if (!KeyMayMatch(ExtractUserKey(internal_key))) {
    return Status::OK();
  }
  BlockIter<> index_iter(index_block_);
  index_iter.Seek(internal_key);
  if (!index_iter.Valid()) {
    return index_iter.status();
  }
  Block block;
  Status s = ReadDataBlock(index_iter.value(), &block);
  if (!s.ok()) {
    return s;
  }
  BlockIter<> iter(block);
  iter.Seek(internal_key);
  if (iter.Valid()) {
    fn(iter.key(), iter.value());
  }
  return iter.status();
}

}  // namespace talus
