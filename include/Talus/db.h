#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Talus/iterator.h"
#include "Talus/options.h"
#include "Talus/pinnable_value.h"
#include "Talus/status.h"
#include "Talus/write_batch.h"

namespace talus {

class DBImpl;

// A consistent read point. Reads with ReadOptions::snapshot see exactly the
// writes committed before GetSnapshot(), and compaction keeps the versions
// they need until ReleaseSnapshot().
class Snapshot {
  struct PrivateTag {};

 public:
  Snapshot(PrivateTag, uint64_t sequence) : sequence_(sequence) {}  // Created by the DB only.

  uint64_t sequence() const { return sequence_; }

 private:
  friend class DBImpl;
  uint64_t sequence_;
};

// Engine statistics (spec §10). Counters on hot paths are kept per thread
// and summed here, so collecting them never adds a shared atomic to a read
// or write.
struct DBStats {
  // Write amplification = (WAL + flush + compaction bytes) / user bytes.
  uint64_t user_bytes_written = 0;  // Serialized WriteBatch operations.
  uint64_t wal_bytes_written = 0;
  uint64_t flush_bytes_written = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  double write_amplification = 0;

  // Read amplification = SSTables consulted per Get (filtered tables
  // count, since their filters were probed).
  uint64_t gets = 0;
  uint64_t tables_consulted = 0;
  double read_amplification = 0;

  // Space amplification = SST bytes / estimated live bytes, where live
  // bytes are estimated as the size of the deepest non-empty level
  // (deduplicated data settles there).
  uint64_t sst_bytes = 0;
  uint64_t estimated_live_bytes = 0;
  double space_amplification = 0;

  std::vector<int> files_per_level;
  std::vector<uint64_t> bytes_per_level;

  uint64_t bloom_checks = 0;
  uint64_t bloom_negatives = 0;  // "Definitely absent".
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t super_version_fast_path = 0;
  uint64_t super_version_slow_path = 0;

  // Group commit: number of groups, and how many had [2^i, 2^(i+1)) members
  // (the last bucket is 64 or more).
  uint64_t write_groups = 0;
  std::array<uint64_t, 7> group_size_histogram{};

  uint64_t stall_count = 0;
  uint64_t stall_micros = 0;
  uint64_t flushes = 0;
  uint64_t flush_micros = 0;
  uint64_t compactions = 0;
  uint64_t compaction_micros = 0;
  uint64_t rate_limiter_wait_micros = 0;
};

// A persistent, ordered key-value store (an LSM tree). All methods are
// thread-safe. No call may run concurrently with destruction, and iterators,
// snapshots and PinnableValues must be released before the DB is destroyed.
class DB {
  struct PrivateTag {};

 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);

  // Use Open(). (Public only so std::make_unique can call it.)
  DB(PrivateTag, std::unique_ptr<DBImpl> impl);

  // Clean shutdown: stops background work. Unflushed writes are in the WAL
  // and are recovered on the next Open.
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& options, std::string_view key);
  // Applies every operation in `batch` atomically.
  Status Write(const WriteOptions& options, WriteBatch* batch);

  // NotFound if the key is absent.
  Status Get(const ReadOptions& options, std::string_view key, std::string* value);
  // Zero-copy: `value` points into engine memory, pinned until it is reset.
  Status Get(const ReadOptions& options, std::string_view key, PinnableValue* value);

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options);

  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot* snapshot);

  // Flushes the memtable to an SSTable and waits for it.
  Status Flush();
  // Compacts the user keys in [begin, end] down to the deepest level that
  // holds any of them. An empty begin or end means unbounded.
  Status CompactRange(std::string_view begin, std::string_view end);

  DBStats GetStats() const;

  // Extension (not in the spec's API): waits until flushes and compactions
  // have caught up. Benchmarks use it to start from a quiescent state.
  Status WaitForBackgroundWork();

 private:
  std::unique_ptr<DBImpl> impl_;
};

}  // namespace talus
