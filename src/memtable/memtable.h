#pragma once

#include <cstddef>
#include <string_view>

#include "Talus/status.h"
#include "db/dbformat.h"
#include "memtable/skiplist.h"
#include "util/arena.h"

namespace talus {

// The in-memory write buffer: a skiplist of (internal key, value) entries.
// Deletions are stored as entries of type kDeletion with an empty value.
class MemTable {
 public:
  using Table = SkipList<InternalKeyComparator>;
  // Yields entries in internal-key order: user key ascending, then sequence
  // descending.
  using Iterator = Table::Iterator;

  // With `concurrent_writes`, Add() may be called from many threads at once
  // (lock-free); otherwise callers must serialize Add() calls. Reads are
  // always lock-free and may run concurrently with writes.
  explicit MemTable(bool concurrent_writes = false, bool use_huge_pages = false);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // REQUIRES: `seq` is not already used by another entry in this memtable,
  // seq <= kMaxSequenceNumber, and key and value are each under 4 GiB.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Looks up the newest entry for `key.user_key()` with sequence <= the
  // lookup key's sequence. If one exists, returns true and sets *s to OK and
  // *value to the stored value (valid for the memtable's lifetime), or sets
  // *s to NotFound if that entry is a deletion. Returns false if the memtable
  // has no visible entry for the key.
  bool Get(const LookupKey& key, std::string_view* value, Status* s) const;

  Iterator NewIterator() const { return Iterator(&table_); }

  // Bytes of arena memory reserved so far; used to decide when to switch to
  // a new memtable.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

 private:
  const bool concurrent_writes_;
  Arena arena_;  // Declared before table_: the table's nodes live in it.
  Table table_;
};

}  // namespace talus
