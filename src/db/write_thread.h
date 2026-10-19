#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Talus/status.h"
#include "Talus/write_batch.h"
#include "db/dbformat.h"

namespace talus {

// The leader/follower protocol behind group commit (spec §5). It only
// coordinates threads: the caller (GroupCommitter) performs the WAL write
// and the memtable inserts.
//
// Writers push themselves onto a lock-free stack (`newest_writer_`). A writer
// that finds the stack empty becomes the leader. Otherwise it waits until a
// departing leader hands it leadership, a leader asks it to insert its own
// batch into the memtable (kParallelMemTableWriter), or a leader finishes its
// write for it (kCompleted).
//
// Invariants:
//  1. At most one group is active at a time. The writers from the leader up
//     to and including `newest_writer_` form a list linked through
//     `link_older` (set by the pusher) and, lazily, `link_newer` (set by
//     leaders walking back from the head). The current leader's
//     `link_older` is null, so the walks stop there.
//  2. Only the departing leader (or the group's last parallel writer acting
//     for it) removes writers from the list, so a failed CAS in exit never
//     needs a retry: it just means more writers arrived.
//  3. Leadership passes with a release/acquire pair: SetState release ->
//     AwaitState acquire, or the exit CAS that empties the stack (release) ->
//     the next pusher's CAS (acquire). Leader-only state (sequence counter,
//     log writer, merge buffer) therefore needs no lock.
//  4. A Writer lives on its thread's stack and may be destroyed as soon as
//     that thread sees kCompleted. Every exit path reads a writer's
//     `link_older` before completing it, and completes the leader last,
//     because the WriteGroup lives on the leader's stack.
//  5. Waiting is lifetime-safe. A waiter that blocks first CASes its state
//     to kLockedWaiting. A setter that finds kLockedWaiting stores
//     (new | kNotifyPending), calls notify_one(), then stores `new`. That
//     final store is its last access to the Writer, and a waiter never
//     returns while kNotifyPending is set. A setter that finds the waiter
//     still spinning changes the state with a single CAS and never touches
//     the Writer again. So notify_one() never runs on a destroyed atomic.
class WriteThread {
 public:
  enum State : uint8_t {
    kInit = 1,
    kGroupLeader = 2,
    kParallelMemTableWriter = 4,
    kCompleted = 8,
    kLockedWaiting = 16,  // The waiter is blocked in std::atomic::wait.
    kNotifyPending = 32,  // A setter is between notify_one() and its final store.
  };

  struct WriteGroup;

  struct Writer {
    Writer(WriteBatch* b, bool s) : batch(b), sync(s) {}

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    WriteBatch* const batch;
    const bool sync;
    Status status;                // Set by whoever applies this writer's batch.
    WriteGroup* group = nullptr;  // Set by the leader that includes this writer.
    std::atomic<uint8_t> state{kInit};
    Writer* link_older = nullptr;
    Writer* link_newer = nullptr;
  };

  struct WriteGroup {
    Writer* leader = nullptr;
    Writer* last = nullptr;  // Newest member.
    size_t size = 0;
    SequenceNumber last_sequence = 0;
    std::atomic<size_t> running{0};  // Members still inserting into the memtable.

    // Calls fn(writer) for each member, oldest (the leader) first.
    template <typename Fn>
    void ForEach(Fn&& fn) const {
      for (Writer* w = leader;; w = w->link_newer) {
        fn(w);
        if (w == last) {
          break;
        }
      }
    }
  };

  // Number of CpuRelax() polls before a waiter blocks in atomic::wait. A
  // group commit typically finishes in a few microseconds (a write(2), or a
  // memtable insert). 256 polls is about 1-10 µs depending on the CPU's
  // PAUSE/YIELD latency, so short waits stay out of the kernel while long
  // ones, such as fsync, block. Experiment 1 in docs/PERFORMANCE.md sweeps
  // this value.
  static constexpr int kDefaultSpinIterations = 256;

  // Writers are merged into a group until it reaches this many bytes. The
  // leader's own batch is always included, even if it is larger.
  static constexpr size_t kMaxGroupBytes = size_t{1} << 20;

  // max_group_bytes = 0 disables grouping: each leader commits only its own
  // batch (the "group commit off" baseline of experiment 1).
  explicit WriteThread(int spin_iterations = kDefaultSpinIterations,
                       size_t max_group_bytes = kMaxGroupBytes)
      : spin_iterations_(spin_iterations), max_group_bytes_(max_group_bytes) {}

  WriteThread(const WriteThread&) = delete;
  WriteThread& operator=(const WriteThread&) = delete;

  // Enqueues `w` and blocks until its state is kGroupLeader,
  // kParallelMemTableWriter or kCompleted. Returns that state.
  uint8_t JoinBatchGroup(Writer* w);

  // Called by a new leader. Builds `group` from the leader and the writers
  // queued behind it, up to max_group_bytes.
  void EnterAsBatchGroupLeader(Writer* leader, WriteGroup* group);

  // Wakes every follower in `group` with kParallelMemTableWriter. Each one,
  // and the leader, then inserts its own batch and calls
  // CompleteParallelMemTableWriter().
  void LaunchParallelMemTableWriters(WriteGroup* group);

  // Marks `w`'s memtable insert done. Returns true if `w` was the group's
  // last running writer; the caller must then publish the group's sequence
  // and call ExitAsBatchGroupLeader(). Otherwise blocks until the group
  // completes, and returns false.
  bool CompleteParallelMemTableWriter(Writer* w);

  // Hands leadership to the next queued writer, if any, then marks every
  // member completed. Members' statuses must already be set. The group must
  // not be used afterwards.
  void ExitAsBatchGroupLeader(WriteGroup* group);

 private:
  uint8_t AwaitState(Writer* w, uint8_t goal_mask) const;
  static void SetState(Writer* w, uint8_t new_state);
  // Fills in `link_newer` from `head` back to the first writer whose
  // `link_newer` is already set, or whose `link_older` is null.
  static void CreateMissingNewerLinks(Writer* head);

  const int spin_iterations_;
  const size_t max_group_bytes_;
  std::atomic<Writer*> newest_writer_{nullptr};
};

}  // namespace talus
