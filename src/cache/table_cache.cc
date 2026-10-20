#include "cache/table_cache.h"

#include <sys/resource.h>

#include <algorithm>
#include <utility>

#include "util/filename.h"

namespace talus {
namespace {

// Each shard gets capacity / shards, rounded down. With few files, one shard
// keeps the total from overshooting max_open_files through rounding.
size_t TableCacheShards(size_t max_open_files) { return max_open_files >= 256 ? 16 : 1; }

// Descriptors left for everything but pread tables: WALs, the MANIFEST, the
// LOCK file, directory syncs, and the rest of the process.
constexpr size_t kReservedFds = 64;

// How many tables may be open at once. A pread table holds a descriptor, so
// the limit is also clamped to the process's soft RLIMIT_NOFILE (as RocksDB
// does); a mapped table holds none.
size_t MaxOpenTables(const TableCache::Options& options) {
  size_t limit = std::max<size_t>(options.max_open_files, 1);
  rlimit fds{};
  if (options.read_mode == RandomAccessFile::Mode::kPread &&
      ::getrlimit(RLIMIT_NOFILE, &fds) == 0 && fds.rlim_cur != RLIM_INFINITY) {
    const auto soft = static_cast<size_t>(fds.rlim_cur);
    limit = std::min(limit, soft > kReservedFds + 1 ? soft - kReservedFds : 1);
  }
  return limit;
}

// Keeps a table open (through its cache handle) for an iterator's lifetime.
class PinnedTableIterator final : public InternalIterator {
 public:
  PinnedTableIterator(Pin table_pin, std::unique_ptr<InternalIterator> iter)
      : table_pin_(std::move(table_pin)), iter_(std::move(iter)) {}

  bool Valid() const override { return iter_->Valid(); }
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  Pin table_pin_;  // Declared first: destroyed after iter_.
  std::unique_ptr<InternalIterator> iter_;
};

// Iterates nothing and reports an error (e.g. the table failed to open).
class ErrorIterator final : public InternalIterator {
 public:
  explicit ErrorIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view /*target*/) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

TableCache::TableCache(std::string dbname, const Options& options)
    : dbname_(std::move(dbname)),
      options_(options),
      max_tables_(MaxOpenTables(options)),
      cache_(max_tables_, TableCacheShards(max_tables_)) {}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
  if ((*handle = cache_.Lookup(file_number)) != nullptr) {
    return Status::OK();
  }
  // Two threads that miss at once both open the file. The second Insert
  // replaces the first entry, which closes once its last pin is released.
  const std::string path = TableFileName(dbname_, file_number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(path, options_.read_mode, &file);
  if (!s.ok()) {
    return s;
  }
  if (file->size() != file_size) {
    return Status::Corruption(path, "sstable size differs from the manifest");
  }
  TableOpenOptions open_options;
  open_options.file_number = file_number;
  open_options.block_cache = options_.block_cache;
  open_options.buffer_pool = options_.buffer_pool;
  std::unique_ptr<Table> table;
  s = Table::Open(open_options, std::move(file), &table);
  if (!s.ok()) {
    return s;  // Failures are not cached, so a repaired file can be retried.
  }
  *handle = cache_.Insert(file_number, std::move(table), /*charge=*/1);
  return Status::OK();
}

std::unique_ptr<InternalIterator> TableCache::NewIterator(uint64_t file_number, uint64_t file_size,
                                                          const TableReadOptions& options) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return std::make_unique<ErrorIterator>(std::move(s));
  }
  Pin pin;
  cache_.PinInto(handle, &pin);
  auto iter = Cache::value(handle)->NewIterator(options);
  return std::make_unique<PinnedTableIterator>(std::move(pin), std::move(iter));
}

Status TableCache::Preload(uint64_t file_number, uint64_t file_size,
                           std::shared_ptr<const PinnedTable>* pinned) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return s;
  }
  if (pinned != nullptr && pinned_tables_.fetch_add(1, std::memory_order_relaxed) < max_tables_) {
    // The PinnedTable takes over our handle reference.
    auto* t = new PinnedTable(this, handle, Cache::value(handle));
    *pinned = std::shared_ptr<const PinnedTable>(t, [](const PinnedTable* p) { p->Unref(); });
    return s;
  }
  if (pinned != nullptr) {
    pinned_tables_.fetch_sub(1, std::memory_order_relaxed);  // Over the limit.
  }
  cache_.Release(handle);
  return s;
}

void PinnedTable::Unref() const {
  if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    TableCache* cache = cache_;
    cache->cache_.Release(static_cast<TableCache::Cache::Handle*>(handle_));
    cache->pinned_tables_.fetch_sub(1, std::memory_order_relaxed);
    delete this;
  }
}

void TableCache::Evict(uint64_t file_number) { cache_.Erase(file_number); }

}  // namespace talus
