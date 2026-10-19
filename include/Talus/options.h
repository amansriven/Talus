#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace talus {

// How SSTable blocks are compressed. The value is stored in each block
// trailer, so it is part of the file format.
enum class CompressionType : uint8_t {
  kNone = 0,
  kLZ4 = 1,  // Only if built with -DTalus_WITH_LZ4=ON.
};

struct Options {
  // As in LevelDB: Open fails unless the database exists or this is set.
  bool create_if_missing = false;
  // Open fails if the database already exists.
  bool error_if_exists = false;

  size_t memtable_size = 4 << 20;
  size_t block_size = 4 << 10;
  int block_restart_interval = 16;
  int bloom_bits_per_key = 10;  // 0 disables the filter.
  size_t block_cache_capacity = 8 << 20;
  int block_cache_shards = 16;
  int max_open_files = 1000;
  uint64_t target_file_size = 2 << 20;
  uint64_t l1_max_bytes = 10 << 20;
  int level_size_multiplier = 10;
  int num_levels = 7;
  int l0_compaction_trigger = 4;
  int l0_slowdown_trigger = 8;
  int l0_stop_trigger = 12;
  int max_background_compactions = 1;
  CompressionType compression = CompressionType::kNone;
  bool paranoid_checks = false;
  bool allow_concurrent_memtable_write = true;
  uint64_t compaction_rate_limit_bytes_per_sec = 0;  // 0 = unlimited.
  bool use_huge_pages = false;                       // MADV_HUGEPAGE for arenas and cache.
  std::vector<int> background_thread_cpus;           // Optional CPU affinity list.

  // Knobs for the A/B experiments in docs/PERFORMANCE.md (not in the spec's
  // option table).
  bool use_blocked_bloom_filter = true;  // Experiment 4: false = standard bloom.
  bool use_mmap_reads = false;           // Experiment 6: mmap instead of pread.
  int write_spin_iterations = 256;       // Group-commit spin before blocking (§5).
  bool enable_group_commit = true;       // Experiment 1: false = one write per group.
  bool use_thread_local_super_version = true;  // Experiment 3: false = mutex per Get.
};

class Snapshot;

struct ReadOptions {
  // Read the state as of this snapshot (from DB::GetSnapshot()); null reads
  // the latest committed state.
  const Snapshot* snapshot = nullptr;
  // Insert blocks read from disk into the block cache. Turn off for large
  // one-time scans, so they do not evict the working set.
  bool fill_cache = true;
};

struct WriteOptions {
  // If true, the write is durable (its WAL record fsynced) before Write()
  // returns. If false, a process crash cannot lose it, but a machine crash
  // can lose the most recent writes.
  bool sync = false;
};

}  // namespace talus
