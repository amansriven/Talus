#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "Talus/options.h"
#include "Talus/status.h"
#include "Talus/write_batch.h"
#include "db/dbformat.h"
#include "db/write_thread.h"

namespace talus {

class MemTable;
namespace log {
class Writer;
}

// Applies WriteBatches with leader/follower group commit (spec §5):
//
//  1. The group's leader assigns each member's batch a consecutive range of
//     sequence numbers.
//  2. It writes all the batches as one WAL record (one write(2)), and does
//     one fsync if any member asked for sync.
//  3. The batches are inserted into the memtable: in parallel, one per
//     member, if the memtable allows concurrent writes; otherwise by the
//     leader.
//  4. The last writer to finish publishes the group's last sequence as
//     visible (release store), then wakes every member with its own status.
//
// Groups are strictly serialized: the next leader starts only after the
// previous group has published, so a visible sequence S always implies that
// every batch with sequence <= S is fully inserted.
//
// After a WAL write or sync fails, the log may hold a partial record, and
// later records would be lost behind it on recovery. So the error is sticky:
// every later Write() returns it.
class GroupCommitter {
 public:
  struct Hooks {
    // Run by each leader before it forms its group, while it holds
    // leadership, so no other write is in progress. The DB uses it to stall
    // writers and to switch to a new memtable and WAL (through SetTarget()).
    // A non-OK result fails the would-be group's writes.
    std::function<Status()> prepare;
    // Run by the leader, on its own thread, after a group's WAL record is
    // written: the number of members and the record's payload size. The DB
    // counts these in the leader's per-thread statistics.
    std::function<void(size_t members, size_t record_bytes)> on_logged;
  };

  // `log` and `mem` must outlive this object, or be replaced through
  // SetTarget() first. `last_sequence` is the newest sequence number already
  // used (e.g. the maximum found by recovery).
  GroupCommitter(log::Writer* log, MemTable* mem, SequenceNumber last_sequence,
                 int spin_iterations = WriteThread::kDefaultSpinIterations, Hooks hooks = {},
                 size_t max_group_bytes = WriteThread::kMaxGroupBytes);

  // Redirects later groups to a new WAL and memtable. Only from PrepareFn:
  // the previous group has finished and the next has not started, so no
  // write is touching either.
  void SetTarget(log::Writer* log, MemTable* mem) {
    log_ = log;
    mem_ = mem;
  }

  GroupCommitter(const GroupCommitter&) = delete;
  GroupCommitter& operator=(const GroupCommitter&) = delete;

  // Thread-safe. Atomically applies `batch` and returns once it is visible
  // (and durable, if options.sync). Overwrites the batch's sequence number.
  Status Write(const WriteOptions& options, WriteBatch* batch);

  // The newest sequence number at which reads see a consistent state.
  SequenceNumber visible_sequence() const {
    // Acquire: pairs with Publish(), so every entry up to the returned
    // sequence is visible in the memtable.
    return visible_sequence_.load(std::memory_order_acquire);
  }

 private:
  Status WriteToLog(const WriteThread::WriteGroup& group, bool need_sync, size_t* record_bytes);
  void FinishParallelWrite(WriteThread::Writer* w);
  void Publish(SequenceNumber seq) { visible_sequence_.store(seq, std::memory_order_release); }

  WriteThread write_thread_;
  const Hooks hooks_;

  // Leader-only state, including log_ and mem_. Leadership handoff in
  // WriteThread provides the release/acquire ordering, so no lock is needed.
  // Parallel followers read mem_ after the leader's SetState(release) wakes
  // them.
  log::Writer* log_;
  MemTable* mem_;
  SequenceNumber last_sequence_;
  Status error_;        // Sticky WAL error.
  std::string merged_;  // Reused buffer for multi-writer WAL records.

  std::atomic<SequenceNumber> visible_sequence_;
};

}  // namespace talus
