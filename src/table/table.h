#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "Talus/status.h"
#include "cache/lru_cache.h"
#include "db/dbformat.h"
#include "table/block.h"
#include "table/filter_policy.h"
#include "table/iterator.h"
#include "util/buffer_pool.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/pin.h"

namespace talus {

// Block-cache entries are keyed by (file number, block offset). File numbers
// are never reused, so a deleted file's blocks simply age out.
struct BlockCacheKey {
  uint64_t file_number = 0;
  uint64_t offset = 0;
  bool operator==(const BlockCacheKey&) const = default;
};

struct BlockCacheKeyHash {
  static uint64_t Hash(const BlockCacheKey& key) {
    char buf[16];
    std::memcpy(buf, &key.file_number, 8);
    std::memcpy(buf + 8, &key.offset, 8);
    return Hash64(buf, sizeof(buf));
  }
};

// Capacity in bytes; each block is charged its size.
using BlockCache = ShardedLRUCache<BlockCacheKey, Block, BlockCacheKeyHash>;

// Counters a read adds to (the DB keeps one set per thread).
struct TableReadStats {
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t filter_checks = 0;
  uint64_t filter_negatives = 0;  // The filter said "definitely absent".
};

struct TableReadOptions {
  bool fill_cache = true;  // Insert blocks read from disk into the block cache.
  TableReadStats* stats = nullptr;
};

struct TableOpenOptions {
  uint64_t file_number = 0;
  BlockCache* block_cache = nullptr;          // May be null: no caching.
  HugePageBufferPool* buffer_pool = nullptr;  // May be null: blocks on the heap.
};

// An open, immutable SSTable. Opening parses the footer and keeps the index
// block and the filter in memory. Data blocks are read on demand, through the
// block cache if there is one. Thread-safe.
//
// In mmap mode, uncompressed data blocks are used in place in the mapping and
// bypass the block cache: the OS page cache already holds them, and a cached
// block pointing into the mapping would dangle once the table was closed.
class Table {
  struct PrivateTag {};

 public:
  static Status Open(const TableOpenOptions& options, std::unique_ptr<RandomAccessFile> file,
                     std::unique_ptr<Table>* table);

  // Use Open(). (Public only so std::make_unique can call it.)
  Table(PrivateTag, const TableOpenOptions& options, std::unique_ptr<RandomAccessFile> file);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // False if the filter proves the table holds no entry for `user_key`.
  bool KeyMayMatch(std::string_view user_key) const {
    return filter_.empty() || FilterMayContain(filter_, Hash64(user_key));
  }

  // Point lookup. Finds the first entry with key >= `internal_key` (normally
  // a LookupKey) and, if there is one, calls fn(key, value, pin). `key` is
  // valid only during the call. `value` stays valid as long as both `pin`
  // and this table are alive, so the callback may move the pin out to keep
  // the value without copying it. Does no I/O if the filter rules the user
  // key out. On a block-cache hit, it does not allocate.
  template <typename Fn>
  Status InternalGet(std::string_view internal_key, const TableReadOptions& options, Fn&& fn) const;

  // Iterates every entry in order. The iterator must not outlive the table.
  std::unique_ptr<InternalIterator> NewIterator(const TableReadOptions& options = {}) const;

  // Fetches the data block an index entry points to: from the block cache,
  // from disk (then cached if options.fill_cache), or in place from the
  // mapping. On success *block points either at a block kept alive by *pin,
  // or at *storage (an mmap block, valid while the table lives).
  Status GetDataBlock(std::string_view index_value, const TableReadOptions& options, Block* storage,
                      Pin* pin, const Block** block) const;

 private:
  const uint64_t file_number_;
  BlockCache* const block_cache_;
  HugePageBufferPool* const buffer_pool_;
  std::unique_ptr<RandomAccessFile> file_;
  Block index_block_;
  // The filter, copied into a 64-byte-aligned buffer so each block of a
  // blocked bloom filter is exactly one cache line.
  std::unique_ptr<char[]> filter_storage_;
  std::string_view filter_;

  friend class TableIterator;
};

template <typename Fn>
Status Table::InternalGet(std::string_view internal_key, const TableReadOptions& options,
                          Fn&& fn) const {
  if (!filter_.empty()) {
    const bool may_match = FilterMayContain(filter_, Hash64(ExtractUserKey(internal_key)));
    if (options.stats != nullptr) {
      ++options.stats->filter_checks;
      options.stats->filter_negatives += may_match ? 0 : 1;
    }
    if (!may_match) {
      return Status::OK();
    }
  }
  BlockIter<> index_iter(index_block_);
  index_iter.Seek(internal_key);
  if (!index_iter.Valid()) {
    return index_iter.status();
  }
  Block storage;
  Pin pin;
  const Block* block = nullptr;
  Status s = GetDataBlock(index_iter.value(), options, &storage, &pin, &block);
  if (!s.ok()) {
    return s;
  }
  BlockIter<> iter(*block);
  iter.Seek(internal_key);
  if (iter.Valid()) {
    fn(iter.key(), iter.value(), pin);
  }
  return iter.status();
}

}  // namespace talus
