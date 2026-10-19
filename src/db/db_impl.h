#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Talus/options.h"
#include "Talus/status.h"
#include "Talus/write_batch.h"
#include "cache/table_cache.h"
#include "db/group_commit.h"
#include "db/super_version.h"
#include "table/table_builder.h"
#include "util/buffer_pool.h"
#include "util/file.h"
#include "util/rate_limiter.h"
#include "version/version_set.h"
#include "wal/log_writer.h"

namespace talus {

// Read-side options for DBImpl::Get. Phase 7's public ReadOptions maps onto
// these.
struct GetOptions {
  SequenceNumber snapshot = kMaxSequenceNumber;  // kMax: read the latest state.
  bool fill_cache = true;
};

// Counters for background work, guarded by the DB mutex, plus summed
// per-thread counters.
struct DBInternalStats {
  uint64_t flushes = 0;
  uint64_t flush_micros = 0;
  uint64_t flush_bytes_written = 0;
  uint64_t compactions = 0;
  uint64_t compaction_micros = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t rate_limiter_wait_micros = 0;
  uint64_t stall_count = 0;
  uint64_t stall_micros = 0;
  uint64_t super_version_fast_path = 0;
  uint64_t super_version_slow_path = 0;
  std::vector<int> files_per_level;
  std::vector<uint64_t> bytes_per_level;
};

// The storage engine: write path, read path, recovery, flush, compaction.
// Thread-safe. No call may run concurrently with destruction.
class DBImpl {
  struct PrivateTag {};

 public:
  static Status Open(const Options& options, const std::string& dbname,
                     std::unique_ptr<DBImpl>* db);

  // Use Open(). (Public only so std::make_unique can call it.)
  DBImpl(PrivateTag, const Options& options, std::string dbname);
  // Stops background work (an in-progress compaction is abandoned; its
  // outputs are deleted on the next open). The memtable is not flushed: its
  // contents are in the WAL and are recovered on reopen.
  ~DBImpl();

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  Status Write(const WriteOptions& options, WriteBatch* batch);
  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& options, std::string_view key);
  Status Get(const GetOptions& options, std::string_view key, std::string* value);

  // Switches to a new memtable (if the current one is not empty), then
  // waits until every memtable written so far has been flushed.
  Status Flush();

  // Flushes, then compacts every file overlapping user keys [begin, end]
  // down to the deepest level that holds any of them. An empty begin or end
  // means unbounded.
  Status CompactRange(std::string_view begin, std::string_view end);

  // A read sequence that compaction preserves until released. Phase 7 wraps
  // these in Snapshot objects.
  SequenceNumber AcquireSnapshot();
  void ReleaseSnapshot(SequenceNumber sequence);

  DBInternalStats GetInternalStats();

  // Blocks until no flush is pending, no compaction is running, and no
  // level needs compacting (or a background error occurs, which it
  // returns). For tests and benchmarks that need a quiescent state.
  Status WaitForBackgroundWork();

 private:
  Status Recover();
  void StartBackgroundThreads();
  std::shared_ptr<MemTable> NewMemTable() const;
  Status CreateLog(uint64_t number, std::unique_ptr<log::Writer>* log);

  // Write path (run by the group-commit leader before each group).
  Status MakeRoomForWrite();

  // Read path.
  SuperVersion* AcquireSuperVersion(ThreadSlot* slot);
  void InstallSuperVersion();  // REQUIRES: mu_ held.

  // Background work. Functions taking a lock require it held, and may
  // release it temporarily.
  void FlushThreadMain();
  void CompactionThreadMain();
  Status BuildTable(const MemTable& mem, uint64_t number, FileMetaData* meta);
  Status FlushImmutable(std::unique_lock<std::mutex>* lock);
  Status RunCompaction(std::unique_ptr<Compaction> c, std::unique_lock<std::mutex>* lock);
  Status DoCompactionWork(Compaction* c, std::unique_lock<std::mutex>* lock);
  void DeleteObsoleteFiles(std::unique_lock<std::mutex>* lock, bool recovering = false);
  void RecordBackgroundError(const Status& s);  // REQUIRES: mu_ held.

  const Options options_;
  const std::string dbname_;
  const TableOptions table_options_;

  // Destroyed last: the database stays locked until everything is closed.
  std::unique_ptr<FileLock> lock_file_;
  std::unique_ptr<HugePageBufferPool> buffer_pool_;  // Before the block cache it backs.
  std::unique_ptr<BlockCache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<RateLimiter> rate_limiter_;

  std::mutex mu_;
  // Signals background progress (flush or compaction finished, error) and
  // new background work. Stalled writers, Flush() and background threads
  // wait on it.
  std::condition_variable bg_cv_;

  std::unique_ptr<VersionSet> versions_;

  // Write state, guarded by mu_ and changed only by the group-commit leader.
  std::unique_ptr<log::Writer> log_;
  uint64_t logfile_number_ = 0;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // Waiting to be flushed; at most one.
  std::unique_ptr<GroupCommitter> committer_;

  // Read state.
  SuperVersion* super_version_ = nullptr;  // Guarded by mu_; holds a reference.
  uint64_t super_version_counter_ = 0;     // Guarded by mu_.
  std::atomic<uint64_t> super_version_generation_{0};
  // Declared after versions_, so its destructor releases threads' cached
  // SuperVersions while the VersionSet still exists.
  PerThreadState per_thread_;

  // Background state, guarded by mu_.
  std::set<uint64_t> pending_outputs_;  // Table files being written.
  std::vector<bool> busy_levels_;       // Levels reserved by running compactions.
  std::multiset<SequenceNumber> snapshots_;
  Status bg_error_;  // Sticky: once set, writes and background work stop.
  bool force_flush_ = false;
  uint64_t memtables_switched_ = 0;
  uint64_t memtables_flushed_ = 0;
  DBInternalStats stats_;
  std::atomic<bool> shutting_down_{false};

  std::thread flush_thread_;
  std::vector<std::thread> compaction_threads_;
};

}  // namespace talus
