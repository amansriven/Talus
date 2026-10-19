#include "db/write_thread.h"

#include <cassert>
#include <thread>

#include "db/write_batch_internal.h"
#include "util/port.h"

namespace talus {

uint8_t WriteThread::JoinBatchGroup(Writer* w) {
  Writer* head = newest_writer_.load(std::memory_order_relaxed);
  do {
    w->link_older = head;
    // Release publishes w's fields to the leader that loads the head.
    // Acquire pairs with the CAS that emptied the stack, if `head` is null:
    // the new leader then sees the previous leader's state (invariant 3).
  } while (!newest_writer_.compare_exchange_weak(head, w, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed));
  if (head == nullptr) {
    // Relaxed: no other thread reads w->state until w publishes something.
    w->state.store(kGroupLeader, std::memory_order_relaxed);
    return kGroupLeader;
  }
  return AwaitState(w, kGroupLeader | kParallelMemTableWriter | kCompleted);
}

void WriteThread::EnterAsBatchGroupLeader(Writer* leader, WriteGroup* group) {
  assert(leader->link_older == nullptr);
  group->leader = leader;
  group->last = leader;
  group->size = 1;
  leader->group = group;
  size_t bytes = WriteBatchInternal::ByteSize(*leader->batch);

  // Acquire: every push up to `newest` is a release in the same RMW chain,
  // so all queued writers' fields are visible.
  Writer* newest = newest_writer_.load(std::memory_order_acquire);
  CreateMissingNewerLinks(newest);
  for (Writer* w = leader; w != newest;) {
    Writer* next = w->link_newer;
    const size_t next_bytes = WriteBatchInternal::ByteSize(*next->batch);
    if (bytes + next_bytes > kMaxGroupBytes) {
      break;
    }
    bytes += next_bytes;
    next->group = group;  // Published to `next` by the SetState that wakes it.
    group->last = next;
    ++group->size;
    w = next;
  }
}

void WriteThread::LaunchParallelMemTableWriters(WriteGroup* group) {
  // Relaxed: followers read `running` only after the SetState release below.
  group->running.store(group->size, std::memory_order_relaxed);
  // Followers cannot finish before the leader decrements `running`, so they
  // are all still alive while we walk their links.
  group->ForEach([&](Writer* w) {
    if (w != group->leader) {
      SetState(w, kParallelMemTableWriter);
    }
  });
}

bool WriteThread::CompleteParallelMemTableWriter(Writer* w) {
  // acq_rel: each decrement releases that writer's memtable inserts and
  // status. The last decrement, an acquire on the same RMW chain, sees all of
  // them before the caller publishes the group's sequence.
  if (w->group->running.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    AwaitState(w, kCompleted);
    return false;
  }
  return true;
}

void WriteThread::ExitAsBatchGroupLeader(WriteGroup* group) {
  // Copy out what we need: the group lives on the leader's stack and can
  // vanish once the leader is completed (invariant 4).
  Writer* const leader = group->leader;
  Writer* const last = group->last;

  Writer* head = newest_writer_.load(std::memory_order_acquire);
  // Release on success: a writer that later finds the stack empty becomes
  // leader and must see this group's leader-only state (invariant 3).
  if (head != last || !newest_writer_.compare_exchange_strong(
                          head, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
    // Writers are queued behind the group. A failed CAS reloads `head`; no
    // retry is needed (invariant 2).
    CreateMissingNewerLinks(head);
    Writer* next_leader = last->link_newer;
    assert(next_leader != nullptr && next_leader->link_older == last);
    next_leader->link_older = nullptr;  // Ends future link walks (invariant 1).
    SetState(next_leader, kGroupLeader);
  }

  for (Writer* w = last; w != leader;) {
    Writer* older = w->link_older;  // Read first: w may be destroyed once completed.
    SetState(w, kCompleted);
    w = older;
  }
  SetState(leader, kCompleted);
}

void WriteThread::CreateMissingNewerLinks(Writer* head) {
  while (true) {
    Writer* older = head->link_older;
    if (older == nullptr || older->link_newer != nullptr) {
      assert(older == nullptr || older->link_newer == head);
      return;
    }
    older->link_newer = head;
    head = older;
  }
}

uint8_t WriteThread::AwaitState(Writer* w, uint8_t goal_mask) const {
  // Acquire throughout: pairs with the release in SetState, so the setter's
  // writes (status, group, sequence) are visible once the goal state is seen.
  uint8_t state = w->state.load(std::memory_order_acquire);
  for (int i = 0; (state & goal_mask) == 0 && i < spin_iterations_; ++i) {
    CpuRelax();
    state = w->state.load(std::memory_order_acquire);
  }
  if ((state & goal_mask) != 0) {
    return state;
  }

  // Announce that we are about to block, so the setter knows to notify. If
  // the CAS fails, the setter already moved us to the goal state.
  if (w->state.compare_exchange_strong(state, kLockedWaiting, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    while (true) {
      w->state.wait(kLockedWaiting, std::memory_order_acquire);
      state = w->state.load(std::memory_order_acquire);
      if (state == kLockedWaiting) {
        continue;  // Spurious wakeup.
      }
      // The setter is still inside notify_one(); returning now could free
      // the atomic under it (invariant 5). The window is a single syscall.
      while ((state & kNotifyPending) != 0) {
        std::this_thread::yield();
        state = w->state.load(std::memory_order_acquire);
      }
      break;
    }
  }
  assert((state & goal_mask) != 0);
  return state;
}

void WriteThread::SetState(Writer* w, uint8_t new_state) {
  uint8_t state = w->state.load(std::memory_order_acquire);
  // Release: publishes everything written for `w` before this call. If the
  // waiter is still spinning, this CAS is our only access to `w`.
  if (state != kLockedWaiting &&
      w->state.compare_exchange_strong(state, new_state, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return;
  }
  // Only the waiter changes the state concurrently with us, and only to
  // kLockedWaiting.
  assert(state == kLockedWaiting);
  w->state.store(new_state | kNotifyPending, std::memory_order_release);
  w->state.notify_one();
  w->state.store(new_state, std::memory_order_release);  // Last access to `w`.
}

}  // namespace talus
