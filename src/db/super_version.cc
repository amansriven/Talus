#include "db/super_version.h"

#include <mutex>
#include <vector>

namespace talus {

void SuperVersion::Unref() {
  // acq_rel: the release half orders this thread's reads of the SuperVersion
  // before the decrement. The acquire half makes every other thread's reads
  // happen-before the deletion by whichever thread drops the last reference.
  if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    const std::unique_ptr<SuperVersion> doomed(this);
  }
}

namespace {

struct ThreadData;

// The global registry of threads' slots. A function-local static, so it is
// constructed before the first ThreadData registers (whose constructor calls
// GetRegistry()). C++ destroys thread_locals before statics, so it outlives
// every ThreadData.
struct Registry {
  std::mutex mu;
  std::vector<ThreadData*> threads;
  std::vector<size_t> free_indexes;
  size_t next_index = 0;
  std::vector<CounterTotals> retired;  // Per DB index.
};

Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

struct ThreadData {
  ThreadData() {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.threads.push_back(this);
  }

  ~ThreadData() {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mu);
    for (size_t i = 0; i < slots.size(); ++i) {
      if (slots[i] == nullptr) {
        continue;
      }
      for (size_t c = 0; c < kNumCounters; ++c) {
        // Relaxed: we hold the registry lock, and this thread wrote them.
        r.retired[i][c] += slots[i]->counters[c].load(std::memory_order_relaxed);
      }
      if (slots[i]->super_version != nullptr) {
        slots[i]->super_version->Unref();
      }
    }
    std::erase(r.threads, this);
  }

  // Indexed by DB index. Resized only by the owning thread, under the lock.
  std::vector<std::unique_ptr<ThreadSlot>> slots;
};

thread_local ThreadData t_data;

}  // namespace

PerThreadState::PerThreadState() {
  Registry& r = GetRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  if (!r.free_indexes.empty()) {
    index_ = r.free_indexes.back();
    r.free_indexes.pop_back();
  } else {
    index_ = r.next_index++;
    r.retired.emplace_back();
  }
  r.retired[index_].fill(0);
}

PerThreadState::~PerThreadState() {
  Registry& r = GetRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  // No thread is using this DB any more (callers must not race with
  // destruction), so its slots can be released from here.
  for (ThreadData* td : r.threads) {
    if (index_ < td->slots.size() && td->slots[index_] != nullptr) {
      if (td->slots[index_]->super_version != nullptr) {
        td->slots[index_]->super_version->Unref();
      }
      td->slots[index_].reset();
    }
  }
  r.retired[index_].fill(0);
  r.free_indexes.push_back(index_);
}

ThreadSlot& PerThreadState::Local() {
  ThreadData& td = t_data;
  // Fast path: no lock. Only this thread resizes td.slots, and a destroying
  // DB touches only its own index, which this thread is not using.
  if (index_ < td.slots.size() && td.slots[index_] != nullptr) {
    return *td.slots[index_];
  }
  Registry& r = GetRegistry();
  std::lock_guard<std::mutex> lock(r.mu);  // Other DBs' destructors walk td.slots.
  if (td.slots.size() <= index_) {
    td.slots.resize(index_ + 1);
  }
  td.slots[index_] = std::make_unique<ThreadSlot>();
  return *td.slots[index_];
}

CounterTotals PerThreadState::Sum() const {
  Registry& r = GetRegistry();
  std::lock_guard<std::mutex> lock(r.mu);
  CounterTotals totals = r.retired[index_];
  for (const ThreadData* td : r.threads) {
    if (index_ < td->slots.size() && td->slots[index_] != nullptr) {
      for (size_t c = 0; c < kNumCounters; ++c) {
        // Relaxed: a statistic; the owner may be mid-update.
        totals[c] += td->slots[index_]->counters[c].load(std::memory_order_relaxed);
      }
    }
  }
  return totals;
}

}  // namespace talus
