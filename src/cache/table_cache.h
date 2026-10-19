#pragma once

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
  Cache cache_;
};

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
