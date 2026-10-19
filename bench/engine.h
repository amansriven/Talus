#pragma once

// A thin interface over the storage engines being compared (spec §12):
// Talus always, and LevelDB and RocksDB when built with
// -DTalus_BENCH_COMPARE=ON. All engines are opened from one EngineConfig, so
// they share matched settings: memtable size, block size, bloom bits per
// key, block cache size, open files, file and level sizes, L0 triggers,
// compression off, and the same sync semantics. The config is written into
// every result file.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace talus::bench {

struct EngineConfig {
  std::string path;
  bool fresh = true;  // Delete any existing database first.
  bool sync = false;  // fsync each write before acknowledging it.

  // Matched across engines.
  uint64_t memtable_size = 4 << 20;
  uint64_t block_size = 4 << 10;
  int bloom_bits_per_key = 10;
  uint64_t block_cache_size = 8 << 20;
  int max_open_files = 1000;
  uint64_t target_file_size = 2 << 20;
  uint64_t l1_max_bytes = 10 << 20;
  int level_size_multiplier = 10;
  int num_levels = 7;
  int l0_compaction_trigger = 4;
  int l0_slowdown_trigger = 8;
  int l0_stop_trigger = 12;
  int max_background_compactions = 1;

  // Talus-only knobs for the A/B experiments.
  bool blocked_bloom = true;           // Experiment 4.
  bool mmap_reads = false;             // Experiment 6.
  bool huge_pages = false;             // Experiment 6.
  bool group_commit = true;            // Experiment 1.
  bool concurrent_memtable = true;     // Experiment 2.
  bool tls_super_version = true;       // Experiment 3.
  uint64_t compaction_rate_limit = 0;  // Experiment 5 (bytes/s, 0 = off).
  int write_spin_iterations = 256;
  std::vector<int> background_cpus;

  std::string ToJson() const;
};

class EngineIterator {
 public:
  virtual ~EngineIterator() = default;
  virtual void Seek(std::string_view target) = 0;
  virtual void SeekToFirst() = 0;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
};

class Engine {
 public:
  virtual ~Engine() = default;

  virtual const char* name() const = 0;
  // Writes honor EngineConfig::sync. Return false on error.
  virtual bool Put(std::string_view key, std::string_view value) = 0;
  virtual bool Delete(std::string_view key) = 0;
  // 1 = found, 0 = not found, -1 = error. Reads without copying where the
  // engine supports it (Talus PinnableValue, RocksDB PinnableSlice).
  virtual int Get(std::string_view key) = 0;
  // Like Get, but copies the value out (YCSB read-modify-write).
  virtual int GetCopy(std::string_view key, std::string* value) = 0;
  virtual std::unique_ptr<EngineIterator> NewIterator() = 0;
  // Waits until flushes and compactions have caught up.
  virtual void WaitForCompactions() = 0;
  // Engine statistics as a JSON value (an object for Talus, the engine's
  // stats text for others).
  virtual std::string StatsJson() = 0;
};

// Names: "talus", "leveldb", "rocksdb". Returns null and sets *error on
// failure (including an engine not compiled in).
std::unique_ptr<Engine> OpenEngine(const std::string& name, const EngineConfig& config,
                                   std::string* error);

// The engines this binary was built with.
std::vector<std::string> AvailableEngines();

// Escapes a string for embedding in JSON.
std::string JsonString(std::string_view s);

}  // namespace talus::bench
