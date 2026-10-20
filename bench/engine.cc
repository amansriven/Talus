#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

#include "Talus/db.h"

#if defined(TALUS_BENCH_COMPARE)
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#endif

namespace talus::bench {

std::string JsonString(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string EngineConfig::ToJson() const {
  std::ostringstream o;
  o << std::boolalpha << "{\"path\":" << JsonString(path)
    << ",\"sync\":" << (sync ? "true" : "false") << ",\"memtable_size\":" << memtable_size
    << ",\"block_size\":" << block_size << ",\"bloom_bits_per_key\":" << bloom_bits_per_key
    << ",\"block_cache_size\":" << block_cache_size << ",\"max_open_files\":" << max_open_files
    << ",\"target_file_size\":" << target_file_size << ",\"l1_max_bytes\":" << l1_max_bytes
    << ",\"level_size_multiplier\":" << level_size_multiplier << ",\"num_levels\":" << num_levels
    << ",\"l0_compaction_trigger\":" << l0_compaction_trigger
    << ",\"l0_slowdown_trigger\":" << l0_slowdown_trigger
    << ",\"l0_stop_trigger\":" << l0_stop_trigger
    << ",\"max_background_compactions\":" << max_background_compactions
    << ",\"compression\":\"none\""
    << ",\"talus\":{\"blocked_bloom\":" << blocked_bloom << ",\"mmap_reads\":" << mmap_reads
    << ",\"huge_pages\":" << huge_pages << ",\"group_commit\":" << group_commit
    << ",\"concurrent_memtable\":" << concurrent_memtable
    << ",\"tls_super_version\":" << tls_super_version
    << ",\"compaction_rate_limit\":" << compaction_rate_limit
    << ",\"write_spin_iterations\":" << write_spin_iterations
    << ",\"write_max_yield_micros\":" << write_max_yield_micros
    << ",\"background_nice\":" << background_nice << "}}";
  return o.str();
}

namespace {

// ---- Talus ------------------------------------------------------------------

class TalusIterator final : public EngineIterator {
 public:
  explicit TalusIterator(std::unique_ptr<Iterator> it) : it_(std::move(it)) {}
  void Seek(std::string_view target) override { it_->Seek(target); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  bool Valid() const override { return it_->Valid(); }
  bool ok() const override { return it_->status().ok(); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return it_->key(); }
  std::string_view value() const override { return it_->value(); }

 private:
  std::unique_ptr<Iterator> it_;
};

class TalusEngine final : public Engine {
 public:
  explicit TalusEngine(const EngineConfig& c) : write_options_{.sync = c.sync} {}

  Status Open(const EngineConfig& c) {
    Options o;
    o.create_if_missing = true;
    o.memtable_size = c.memtable_size;
    o.block_size = c.block_size;
    o.bloom_bits_per_key = c.bloom_bits_per_key;
    o.block_cache_capacity = c.block_cache_size;
    o.max_open_files = c.max_open_files;
    o.target_file_size = c.target_file_size;
    o.l1_max_bytes = c.l1_max_bytes;
    o.level_size_multiplier = c.level_size_multiplier;
    o.num_levels = c.num_levels;
    o.l0_compaction_trigger = c.l0_compaction_trigger;
    o.l0_slowdown_trigger = c.l0_slowdown_trigger;
    o.l0_stop_trigger = c.l0_stop_trigger;
    o.max_background_compactions = c.max_background_compactions;
    o.use_blocked_bloom_filter = c.blocked_bloom;
    o.use_mmap_reads = c.mmap_reads;
    o.use_huge_pages = c.huge_pages;
    o.enable_group_commit = c.group_commit;
    o.allow_concurrent_memtable_write = c.concurrent_memtable;
    o.use_thread_local_super_version = c.tls_super_version;
    o.compaction_rate_limit_bytes_per_sec = c.compaction_rate_limit;
    o.write_spin_iterations = c.write_spin_iterations;
    o.write_max_yield_micros = c.write_max_yield_micros;
    o.background_thread_cpus = c.background_cpus;
    o.background_thread_nice = c.background_nice;
    return DB::Open(o, c.path, &db_);
  }

  const char* name() const override { return "talus"; }
  bool Put(std::string_view k, std::string_view v) override {
    return db_->Put(write_options_, k, v).ok();
  }
  bool Delete(std::string_view k) override { return db_->Delete(write_options_, k).ok(); }
  int Get(std::string_view k) override {
    thread_local PinnableValue value;  // Reused: the zero-allocation read path.
    const Status s = db_->Get({}, k, &value);
    return s.ok() ? 1 : (s.IsNotFound() ? 0 : -1);
  }
  int GetCopy(std::string_view k, std::string* v) override {
    const Status s = db_->Get({}, k, v);
    return s.ok() ? 1 : (s.IsNotFound() ? 0 : -1);
  }
  std::unique_ptr<EngineIterator> NewIterator() override {
    return std::make_unique<TalusIterator>(db_->NewIterator({}));
  }
  void WaitForCompactions() override { (void)db_->WaitForBackgroundWork(); }

  std::string StatsJson() override {
    const DBStats s = db_->GetStats();
    std::ostringstream o;
    o << "{\"user_bytes_written\":" << s.user_bytes_written
      << ",\"wal_bytes_written\":" << s.wal_bytes_written
      << ",\"flush_bytes_written\":" << s.flush_bytes_written
      << ",\"compaction_bytes_written\":" << s.compaction_bytes_written
      << ",\"compaction_bytes_read\":" << s.compaction_bytes_read
      << ",\"write_amplification\":" << s.write_amplification << ",\"gets\":" << s.gets
      << ",\"tables_consulted\":" << s.tables_consulted
      << ",\"read_amplification\":" << s.read_amplification << ",\"sst_bytes\":" << s.sst_bytes
      << ",\"estimated_live_bytes\":" << s.estimated_live_bytes
      << ",\"space_amplification\":" << s.space_amplification << ",\"files_per_level\":[";
    for (size_t i = 0; i < s.files_per_level.size(); ++i) {
      o << (i > 0 ? "," : "") << s.files_per_level[i];
    }
    o << "],\"bytes_per_level\":[";
    for (size_t i = 0; i < s.bytes_per_level.size(); ++i) {
      o << (i > 0 ? "," : "") << s.bytes_per_level[i];
    }
    o << "],\"bloom_checks\":" << s.bloom_checks << ",\"bloom_negatives\":" << s.bloom_negatives
      << ",\"block_cache_hits\":" << s.block_cache_hits
      << ",\"block_cache_misses\":" << s.block_cache_misses
      << ",\"super_version_fast_path\":" << s.super_version_fast_path
      << ",\"super_version_slow_path\":" << s.super_version_slow_path
      << ",\"write_groups\":" << s.write_groups << ",\"group_size_histogram\":[";
    for (size_t i = 0; i < s.group_size_histogram.size(); ++i) {
      o << (i > 0 ? "," : "") << s.group_size_histogram[i];
    }
    o << "],\"stall_count\":" << s.stall_count << ",\"stall_micros\":" << s.stall_micros
      << ",\"flushes\":" << s.flushes << ",\"flush_micros\":" << s.flush_micros
      << ",\"compactions\":" << s.compactions << ",\"compaction_micros\":" << s.compaction_micros
      << ",\"rate_limiter_wait_micros\":" << s.rate_limiter_wait_micros << "}";
    return o.str();
  }

 private:
  const WriteOptions write_options_;
  std::unique_ptr<DB> db_;
};

#if defined(TALUS_BENCH_COMPARE)

// ---- LevelDB ------------------------------------------------------------------
// LevelDB's L0 triggers (4/8/12) and level sizing are compile-time
// constants; they equal Talus's defaults.

class LevelDBIterator final : public EngineIterator {
 public:
  explicit LevelDBIterator(leveldb::Iterator* it) : it_(it) {}
  void Seek(std::string_view t) override { it_->Seek(leveldb::Slice(t.data(), t.size())); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  bool Valid() const override { return it_->Valid(); }
  bool ok() const override { return it_->status().ok(); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return {it_->key().data(), it_->key().size()}; }
  std::string_view value() const override { return {it_->value().data(), it_->value().size()}; }

 private:
  std::unique_ptr<leveldb::Iterator> it_;
};

class LevelDBEngine final : public Engine {
 public:
  bool Open(const EngineConfig& c, std::string* error) {
    write_options_.sync = c.sync;
    cache_.reset(leveldb::NewLRUCache(c.block_cache_size));
    filter_.reset(leveldb::NewBloomFilterPolicy(c.bloom_bits_per_key));
    leveldb::Options o;
    o.create_if_missing = true;
    o.write_buffer_size = c.memtable_size;
    o.block_size = c.block_size;
    o.block_cache = cache_.get();
    o.filter_policy = c.bloom_bits_per_key > 0 ? filter_.get() : nullptr;
    o.max_open_files = c.max_open_files;
    o.max_file_size = c.target_file_size;
    o.compression = leveldb::kNoCompression;
    leveldb::DB* db = nullptr;
    const leveldb::Status s = leveldb::DB::Open(o, c.path, &db);
    db_.reset(db);
    if (!s.ok()) {
      *error = s.ToString();
    }
    return s.ok();
  }

  const char* name() const override { return "leveldb"; }
  bool Put(std::string_view k, std::string_view v) override {
    return db_->Put(write_options_, {k.data(), k.size()}, {v.data(), v.size()}).ok();
  }
  bool Delete(std::string_view k) override {
    return db_->Delete(write_options_, {k.data(), k.size()}).ok();
  }
  int Get(std::string_view k) override {
    thread_local std::string value;  // LevelDB has no zero-copy Get.
    return GetCopy(k, &value);
  }
  int GetCopy(std::string_view k, std::string* v) override {
    const leveldb::Status s = db_->Get({}, {k.data(), k.size()}, v);
    return s.ok() ? 1 : (s.IsNotFound() ? 0 : -1);
  }
  std::unique_ptr<EngineIterator> NewIterator() override {
    return std::make_unique<LevelDBIterator>(db_->NewIterator({}));
  }
  void WaitForCompactions() override {
    // LevelDB has no "wait"; poll until no level wants compacting.
    std::string files;
    for (int i = 0; i < 6000; ++i) {
      if (db_->GetProperty("leveldb.num-files-at-level0", &files) && std::stoi(files) < 4) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::string StatsJson() override {
    std::string stats;
    db_->GetProperty("leveldb.stats", &stats);
    return JsonString(stats);
  }

 private:
  leveldb::WriteOptions write_options_;
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<const leveldb::FilterPolicy> filter_;
  std::unique_ptr<leveldb::DB> db_;  // Declared last: closed before cache_ and filter_.
};

// ---- RocksDB ------------------------------------------------------------------

class RocksDBIterator final : public EngineIterator {
 public:
  explicit RocksDBIterator(rocksdb::Iterator* it) : it_(it) {}
  void Seek(std::string_view t) override { it_->Seek(rocksdb::Slice(t.data(), t.size())); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  bool Valid() const override { return it_->Valid(); }
  bool ok() const override { return it_->status().ok(); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return {it_->key().data(), it_->key().size()}; }
  std::string_view value() const override { return {it_->value().data(), it_->value().size()}; }

 private:
  std::unique_ptr<rocksdb::Iterator> it_;
};

class RocksDBEngine final : public Engine {
 public:
  bool Open(const EngineConfig& c, std::string* error) {
    write_options_.sync = c.sync;
    rocksdb::BlockBasedTableOptions table;
    table.block_size = c.block_size;
    table.block_cache = rocksdb::NewLRUCache(c.block_cache_size, /*num_shard_bits=*/4);
    if (c.bloom_bits_per_key > 0) {
      // RocksDB's default full-filter bloom is itself cache-local.
      table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(c.bloom_bits_per_key));
    }
    rocksdb::Options o;
    o.create_if_missing = true;
    o.write_buffer_size = c.memtable_size;
    o.max_write_buffer_number = 2;  // Active + one immutable, as in Talus.
    o.target_file_size_base = c.target_file_size;
    o.level_compaction_dynamic_level_bytes = false;
    o.max_bytes_for_level_base = c.l1_max_bytes;
    o.max_bytes_for_level_multiplier = c.level_size_multiplier;
    o.num_levels = c.num_levels;
    o.level0_file_num_compaction_trigger = c.l0_compaction_trigger;
    o.level0_slowdown_writes_trigger = c.l0_slowdown_trigger;
    o.level0_stop_writes_trigger = c.l0_stop_trigger;
    o.max_background_jobs = c.max_background_compactions + 1;  // Plus one flush.
    o.max_open_files = c.max_open_files;
    o.compression = rocksdb::kNoCompression;
    o.bottommost_compression = rocksdb::kNoCompression;
    o.allow_concurrent_memtable_write = c.concurrent_memtable;
    // LevelDB always maps tables on 64-bit hosts, so the comparison maps
    // them in every engine unless --mmap_reads=0.
    o.allow_mmap_reads = c.mmap_reads;
    o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    rocksdb::DB* db = nullptr;
    const rocksdb::Status s = rocksdb::DB::Open(o, c.path, &db);
    db_.reset(db);
    if (!s.ok()) {
      *error = s.ToString();
    }
    return s.ok();
  }

  const char* name() const override { return "rocksdb"; }
  bool Put(std::string_view k, std::string_view v) override {
    return db_->Put(write_options_, {k.data(), k.size()}, {v.data(), v.size()}).ok();
  }
  bool Delete(std::string_view k) override {
    return db_->Delete(write_options_, {k.data(), k.size()}).ok();
  }
  int Get(std::string_view k) override {
    thread_local rocksdb::PinnableSlice value;  // RocksDB's zero-copy read.
    value.Reset();
    const rocksdb::Status s =
        db_->Get({}, db_->DefaultColumnFamily(), {k.data(), k.size()}, &value);
    return s.ok() ? 1 : (s.IsNotFound() ? 0 : -1);
  }
  int GetCopy(std::string_view k, std::string* v) override {
    const rocksdb::Status s = db_->Get({}, {k.data(), k.size()}, v);
    return s.ok() ? 1 : (s.IsNotFound() ? 0 : -1);
  }
  std::unique_ptr<EngineIterator> NewIterator() override {
    return std::make_unique<RocksDBIterator>(db_->NewIterator({}));
  }
  void WaitForCompactions() override { (void)db_->WaitForCompact({}); }
  std::string StatsJson() override {
    std::string stats;
    db_->GetProperty("rocksdb.stats", &stats);
    return JsonString(stats);
  }

 private:
  rocksdb::WriteOptions write_options_;
  std::unique_ptr<rocksdb::DB> db_;
};

#endif  // TALUS_BENCH_COMPARE

}  // namespace

std::vector<std::string> AvailableEngines() {
#if defined(TALUS_BENCH_COMPARE)
  return {"talus", "leveldb", "rocksdb"};
#else
  return {"talus"};
#endif
}

std::unique_ptr<Engine> OpenEngine(const std::string& name, const EngineConfig& config,
                                   std::string* error) {
  const auto engines = AvailableEngines();
  if (std::find(engines.begin(), engines.end(), name) == engines.end()) {
    *error = "unknown or not built-in engine: " + name;
    return nullptr;
  }
  if (config.fresh) {
    std::error_code ec;
    std::filesystem::remove_all(config.path, ec);
    if (ec) {
      *error = ec.message();
      return nullptr;
    }
  }
  const auto parent = std::filesystem::path(config.path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      *error = ec.message();
      return nullptr;
    }
  }
  if (name == "talus") {
    auto engine = std::make_unique<TalusEngine>(config);
    const Status s = engine->Open(config);
    if (!s.ok()) {
      *error = s.ToString();
      return nullptr;
    }
    return engine;
  }
#if defined(TALUS_BENCH_COMPARE)
  if (name == "leveldb") {
    auto engine = std::make_unique<LevelDBEngine>();
    return engine->Open(config, error) ? std::move(engine) : nullptr;
  }
  if (name == "rocksdb") {
    auto engine = std::make_unique<RocksDBEngine>();
    return engine->Open(config, error) ? std::move(engine) : nullptr;
  }
#endif
  *error = "unknown or not built-in engine: " + name + " (configure with -DTalus_BENCH_COMPARE=ON)";
  return nullptr;
}

}  // namespace talus::bench
