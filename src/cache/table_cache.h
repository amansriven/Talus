#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Talus/status.h"
#include "cache/lru_cache.h"
#include "table/table.h"
#include "util/file.h"
#include "util/pin.h"

namespace talus {

class TableCache;

// A table held open for as long as a Version's FileMetaData refers to it, so
// point lookups reach the Table directly: no hash lookup and no shard mutex.
// One atomic counts its references: the FileMetaData's shared_ptr holds one,
// and each value handed to a caller holds another (PinInto), so a value
// outlives its Version, its file and the table cache's eviction.
class PinnedTable {
 public:
  PinnedTable(const PinnedTable&) = delete;
  PinnedTable& operator=(const PinnedTable&) = delete;

  const Table& table() const { return *table_; }

  // Keeps the table open until `pin` is released.
  void PinInto(Pin* pin) const {
    refs_.fetch_add(1, std::memory_order_relaxed);  // The caller holds a reference.
    pin->Add([](void* t, void*) { static_cast<const PinnedTable*>(t)->Unref(); },
             const_cast<PinnedTable*>(this), nullptr);
  }

 private:
  friend class TableCache;
  PinnedTable(TableCache* cache, void* handle, const Table* table)
      : cache_(cache), handle_(handle), table_(table) {}
  ~PinnedTable() = default;
  // acq_rel: the last reference sees every reader's accesses before it
  // releases the cache handle, which may close the table.
  void Unref() const;

  TableCache* const cache_;
  void* const handle_;  // TableCache::Cache::Handle*.
  const Table* const table_;
  mutable std::atomic<int> refs_{1};
};

// An LRU cache of open SSTable readers (file descriptor or mapping, parsed
// index, filter), bounded by max_open_files. Thread-safe.
class TableCache {
 public:
  struct Options {
    RandomAccessFile::Mode read_mode = RandomAccessFile::Mode::kPread;
    BlockCache* block_cache = nullptr;
    HugePageBufferPool* buffer_pool = nullptr;
    size_t max_open_files = 1000;
  };

  TableCache(std::string dbname, const Options& options);

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Point lookup in table `file_number` (see Table::InternalGet). The pin
  // passed to fn(key, value, pin) keeps `value` valid by itself: it holds
  // both the data block and the table.
  template <typename Fn>
  Status Get(uint64_t file_number, uint64_t file_size, std::string_view internal_key,
             const TableReadOptions& options, Fn&& fn);

  // Iterates table `file_number`; the iterator keeps the table open.
  std::unique_ptr<InternalIterator> NewIterator(uint64_t file_number, uint64_t file_size,
                                                const TableReadOptions& options = {});

  // Opens table `file_number` into the cache, validating its footer, index
  // and filter. Flush and compaction call it on every new table before
  // installing it, so readers never pay for the open (LevelDB and RocksDB do
  // the same when they verify a new table). If `pinned` is not null, it also
  // pins the table for the file's FileMetaData; *pinned stays null once
  // max_open_files tables are pinned, so the open-file bound still holds and
  // reads of that file use the cache.
  Status Preload(uint64_t file_number, uint64_t file_size,
                 std::shared_ptr<const PinnedTable>* pinned = nullptr);

  // Point lookup through a pinned table (see Get): no lookup, no lock.
  template <typename Fn>
  static Status Get(const PinnedTable& table, std::string_view internal_key,
                    const TableReadOptions& options, Fn&& fn);

  // Closes the table, if open (call once its file is deleted). Readers that
  // still pin it keep it open until they finish.
  void Evict(uint64_t file_number);

 private:
  struct NumberHash {
    static uint64_t Hash(uint64_t n) {
      return Hash64(reinterpret_cast<const char*>(&n), sizeof(n));
    }
  };
  using Cache = ShardedLRUCache<uint64_t, Table, NumberHash>;

  // Returns a pinned handle to the open table, opening it on a miss. Returns
  // Corruption if the file's size differs from `file_size`, the size recorded
  // in the manifest.
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle);

  const std::string dbname_;
  const Options options_;
  const size_t max_tables_;  // max_open_files, clamped to the fd limit for pread.
  Cache cache_;
  std::atomic<size_t> pinned_tables_{0};

  friend class PinnedTable;
};

template <typename Fn>
Status TableCache::Get(const PinnedTable& table, std::string_view internal_key,
                       const TableReadOptions& options, Fn&& fn) {
  return table.table().InternalGet(internal_key, options,
                                   [&](std::string_view key, std::string_view value, Pin& pin) {
                                     table.PinInto(&pin);
                                     fn(key, value, pin);
                                   });
}

template <typename Fn>
Status TableCache::Get(uint64_t file_number, uint64_t file_size, std::string_view internal_key,
                       const TableReadOptions& options, Fn&& fn) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return s;
  }
  s = Cache::value(handle)->InternalGet(
      internal_key, options, [&](std::string_view key, std::string_view value, Pin& pin) {
        // Give the value's pin its own reference to the table, so it stays
        // valid after this lookup releases `handle`.
        cache_.Ref(handle);
        cache_.PinInto(handle, &pin);
        fn(key, value, pin);
      });
  cache_.Release(handle);
  return s;
}

}  // namespace talus
