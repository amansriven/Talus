#include "db/group_commit.h"

#include <utility>

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "wal/log_writer.h"

namespace talus {

GroupCommitter::GroupCommitter(log::Writer* log, MemTable* mem, SequenceNumber last_sequence,
                               int spin_iterations, Hooks hooks,
                               size_t max_group_bytes)
    : write_thread_(spin_iterations, max_group_bytes),
      hooks_(std::move(hooks)),
      log_(log),
      mem_(mem),
      last_sequence_(last_sequence),
      visible_sequence_(last_sequence) {}

Status GroupCommitter::Write(const WriteOptions& options, WriteBatch* batch) {
  WriteThread::Writer w(batch, options.sync);
  const uint8_t state = write_thread_.JoinBatchGroup(&w);
  if (state == WriteThread::kCompleted) {
    return w.status;  // A leader applied our batch.
  }
  if (state == WriteThread::kParallelMemTableWriter) {
    w.status = WriteBatchInternal::InsertInto(WriteBatchInternal::Contents(*batch), mem_);
    FinishParallelWrite(&w);
    return w.status;
  }

  // We lead. Prepare first, so writers arriving during a stall still join
  // this group.
  Status s = error_;
  if (s.ok() && hooks_.prepare) {
    s = hooks_.prepare();
  }
  WriteThread::WriteGroup group;
  write_thread_.EnterAsBatchGroupLeader(&w, &group);

  if (s.ok()) {
    SequenceNumber next = last_sequence_ + 1;
    bool need_sync = false;
    group.ForEach([&](WriteThread::Writer* m) {
      WriteBatchInternal::SetSequence(m->batch, next);
      next += WriteBatchInternal::Count(*m->batch);
      need_sync = need_sync || m->sync;
    });
    size_t record_bytes = 0;
    s = WriteToLog(group, need_sync, &record_bytes);
    if (s.ok()) {
      last_sequence_ = next - 1;
      group.last_sequence = last_sequence_;
      if (hooks_.on_logged) {
        hooks_.on_logged(group.size, record_bytes);
      }
    } else {
      error_ = s;
    }
  }
  if (!s.ok()) {
    group.ForEach([&](WriteThread::Writer* m) { m->status = s; });
    write_thread_.ExitAsBatchGroupLeader(&group);
    return s;
  }

  if (mem_->concurrent_writes() && group.size > 1) {
    write_thread_.LaunchParallelMemTableWriters(&group);
    w.status = WriteBatchInternal::InsertInto(WriteBatchInternal::Contents(*batch), mem_);
    FinishParallelWrite(&w);
    return w.status;
  }

  group.ForEach([&](WriteThread::Writer* m) {
    m->status = WriteBatchInternal::InsertInto(WriteBatchInternal::Contents(*m->batch), mem_);
  });
  Publish(group.last_sequence);
  write_thread_.ExitAsBatchGroupLeader(&group);
  return w.status;
}

void GroupCommitter::FinishParallelWrite(WriteThread::Writer* w) {
  if (write_thread_.CompleteParallelMemTableWriter(w)) {
    // Last writer of the group: every member's inserts are visible to us.
    Publish(w->group->last_sequence);
    write_thread_.ExitAsBatchGroupLeader(w->group);
  }
}

Status GroupCommitter::WriteToLog(const WriteThread::WriteGroup& group, bool need_sync,
                                  size_t* record_bytes) {
  std::string_view record;
  if (group.size == 1) {
    record = WriteBatchInternal::Contents(*group.leader->batch);
  } else {
    // One batch covering the whole group: the leader's header (with the
    // group's first sequence), the summed count, then every member's ops.
    // The members' ranges are consecutive, so replaying the merged batch
    // reproduces exactly the sequences assigned above.
    merged_.assign(WriteBatchInternal::Contents(*group.leader->batch));
    uint32_t count = WriteBatchInternal::Count(*group.leader->batch);
    group.ForEach([&](WriteThread::Writer* m) {
      if (m != group.leader) {
        merged_.append(
            WriteBatchInternal::Contents(*m->batch).substr(WriteBatchInternal::kHeaderSize));
        count += WriteBatchInternal::Count(*m->batch);
      }
    });
    EncodeFixed32(merged_.data() + 8, count);
    record = merged_;
  }
  *record_bytes = record.size();
  Status s = log_->AddRecord(record);
  if (s.ok() && need_sync) {
    s = log_->Sync();
  }
  return s;
}

}  // namespace talus
