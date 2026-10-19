// Allocation test (spec §9). This binary replaces the global operator
// new/delete with counting versions (linker wrappers under Linux TSan),
// then asserts that each hot path makes
// zero heap allocations across 100K operations after a warm-up:
//  * Put and Write (memtable arena growth and WAL file growth are excluded
//    by construction: no memtable switch happens during the measured loop,
//    and arena blocks come from aligned_alloc, which is not counted);
//  * Get into a PinnableValue on a memtable hit and on a block-cache hit.
// It also checks that steady-state Gets never take the SuperVersion slow
// path, which is the one that locks the DB mutex.
//
// Counts are per thread, so the measured thread's allocations are not mixed
// with those of background threads.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "Talus/db.h"
#include "test_util.h"

namespace {

thread_local long t_allocations = 0;

#if !defined(TALUS_ALLOC_WRAP_NEW)
void* CountedAlloc(std::size_t n, std::size_t align) {
  ++t_allocations;
  void* p = nullptr;
  if (align <= alignof(std::max_align_t)) {
    p = std::malloc(n == 0 ? 1 : n);
  } else if (posix_memalign(&p, align, n == 0 ? 1 : n) != 0) {
    p = nullptr;
  }
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
#endif

}  // namespace

#if defined(TALUS_ALLOC_WRAP_NEW)
// Linux TSan owns strong new/delete definitions. --wrap redirects calls from
// the test and engine to these counters, then __real_ forwards to TSan's
// allocation entry points. Do not replace/delete or bypass its allocator.
// Aligned arena backing allocations remain excluded, as in the regular test.
#define TALUS_WRAP_NEW(symbol, params, args) \
  extern "C" void* __real_##symbol params;   \
  extern "C" void* __wrap_##symbol params {  \
    ++t_allocations;                         \
    return __real_##symbol args;             \
  }

TALUS_WRAP_NEW(_Znwm, (std::size_t n), (n))
TALUS_WRAP_NEW(_Znam, (std::size_t n), (n))
TALUS_WRAP_NEW(_ZnwmSt11align_val_t, (std::size_t n, std::align_val_t a), (n, a))
TALUS_WRAP_NEW(_ZnamSt11align_val_t, (std::size_t n, std::align_val_t a), (n, a))
TALUS_WRAP_NEW(_ZnwmRKSt9nothrow_t, (std::size_t n, const std::nothrow_t& t), (n, t))
TALUS_WRAP_NEW(_ZnamRKSt9nothrow_t, (std::size_t n, const std::nothrow_t& t), (n, t))
#undef TALUS_WRAP_NEW
#else
// NOLINTBEGIN: replacing the global allocation functions is the point here.
void* operator new(std::size_t n) { return CountedAlloc(n, 0); }
void* operator new[](std::size_t n) { return CountedAlloc(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) {
  return CountedAlloc(n, static_cast<size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return CountedAlloc(n, static_cast<size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return CountedAlloc(n, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
// NOLINTEND
#endif

namespace talus {
namespace {

constexpr int kOps = 100'000;
constexpr int kKeys = 20'000;

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "user%08d", i % kKeys);
  return buf;
}

template <typename Fn>
long AllocationsDuring(Fn&& fn) {
  const long before = t_allocations;
  fn();
  return t_allocations - before;
}

// A broken/intercepted-away counter could otherwise make every zero-allocation
// assertion pass vacuously. Volatile function pointers prevent allocation
// elision and exercise each allocation entry point we count.
TEST(AllocationCounterTest, CountsEveryNewOverload) {
  void* (*volatile scalar)(std::size_t) = &::operator new;
  void* (*volatile array)(std::size_t) = &::operator new[];
  void* (*volatile aligned)(std::size_t, std::align_val_t) = &::operator new;
  void* (*volatile aligned_array)(std::size_t, std::align_val_t) = &::operator new[];
  void* (*volatile nothrow)(std::size_t, const std::nothrow_t&) noexcept = &::operator new;
  void* (*volatile nothrow_array)(std::size_t, const std::nothrow_t&) noexcept = &::operator new[];
  constexpr auto alignment = std::align_val_t{64};
  const long before = t_allocations;
  void* p1 = scalar(128);
  void* p2 = array(128);
  void* p3 = aligned(128, alignment);
  void* p4 = aligned_array(128, alignment);
  void* p5 = nothrow(128, std::nothrow);
  void* p6 = nothrow_array(128, std::nothrow);
  const long counted = t_allocations - before;
  ::operator delete(p1);
  ::operator delete[](p2);
  ::operator delete(p3, alignment);
  ::operator delete[](p4, alignment);
  ::operator delete(p5);
  ::operator delete[](p6);
  EXPECT_EQ(counted, 6);
}

class AllocTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Options o;
    o.create_if_missing = true;
    o.memtable_size = 256 << 20;  // No memtable switch while measuring.
    ASSERT_TRUE(DB::Open(o, dir_.path(), &db_).ok());
    // Precompute keys so building them is not counted.
    for (int i = 0; i < kKeys; ++i) {
      keys_.push_back(Key(i));
    }
  }

  test::ScopedTempDir dir_;
  std::unique_ptr<DB> db_;
  std::vector<std::string> keys_;
  const std::string value_ = std::string(100, 'v');
};

TEST_F(AllocTest, PutIsAllocationFree) {
  for (int i = 0; i < kKeys; ++i) {  // Warm up: thread slot, batch capacity.
    ASSERT_TRUE(db_->Put({}, keys_[i], value_).ok());
  }
  EXPECT_EQ(AllocationsDuring([&] {
              for (int i = 0; i < kOps; ++i) {
                (void)db_->Put({}, keys_[i % kKeys], value_);
              }
            }),
            0);
}

TEST_F(AllocTest, WriteWithAReusedBatchIsAllocationFree) {
  WriteBatch batch;
  for (int i = 0; i < 1000; ++i) {
    batch.Clear();
    batch.Put(keys_[i], value_);
    batch.Delete(keys_[i + 1]);
    ASSERT_TRUE(db_->Write({}, &batch).ok());
  }
  EXPECT_EQ(AllocationsDuring([&] {
              for (int i = 0; i < kOps; ++i) {
                batch.Clear();
                batch.Put(keys_[i % kKeys], value_);
                batch.Delete(keys_[(i + 7) % kKeys]);
                (void)db_->Write({}, &batch);
              }
            }),
            0);
}

TEST_F(AllocTest, PinnedGetOnAMemtableHitIsAllocationFree) {
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db_->Put({}, keys_[i], value_).ok());
  }
  PinnableValue value;
  ASSERT_TRUE(db_->Get({}, keys_[0], &value).ok());
  EXPECT_EQ(AllocationsDuring([&] {
              for (int i = 0; i < kOps; ++i) {
                (void)db_->Get({}, keys_[i % kKeys], &value);
              }
            }),
            0);
  EXPECT_EQ(value.value(), value_);
}

TEST_F(AllocTest, PinnedGetOnABlockCacheHitIsAllocationFree) {
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db_->Put({}, keys_[i], value_).ok());
  }
  ASSERT_TRUE(db_->CompactRange("", "").ok());  // All data in SSTables.
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  PinnableValue value;
  for (int i = 0; i < kKeys; ++i) {  // Warm the block cache.
    ASSERT_TRUE(db_->Get({}, keys_[i], &value).ok());
  }
  const DBStats before = db_->GetStats();
  EXPECT_EQ(AllocationsDuring([&] {
              for (int i = 0; i < kOps; ++i) {
                (void)db_->Get({}, keys_[(i * 7919) % kKeys], &value);
              }
            }),
            0);
  const DBStats after = db_->GetStats();
  EXPECT_EQ(after.block_cache_misses, before.block_cache_misses);  // Really cache hits.
  EXPECT_EQ(after.block_cache_hits - before.block_cache_hits, static_cast<uint64_t>(kOps));
}

// Spec §9: no mutex on the Get fast path. Once each reader thread has cached
// the current SuperVersion, a read-only workload must never take the slow
// (mutex) path.
TEST_F(AllocTest, ReadOnlySteadyStateNeverTakesTheSlowPath) {
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db_->Put({}, keys_[i], value_).ok());
  }
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  constexpr int kReaders = 4;
  std::vector<std::thread> threads;
  std::atomic<int> warmed{0};
  std::atomic<bool> go{false};
  DBStats before;
  for (int t = 0; t < kReaders; ++t) {
    threads.emplace_back([&, t] {
      std::string value;
      (void)db_->Get({}, keys_[t], &value);  // First Get: the one slow path.
      // Release/acquire: the warm-up Get happens before the main thread's
      // `before` snapshot of the stats.
      warmed.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kOps; ++i) {
        ASSERT_TRUE(db_->Get({}, keys_[(i * 31 + t) % kKeys], &value).ok());
      }
    });
  }
  while (warmed.load(std::memory_order_acquire) < kReaders) {
    std::this_thread::yield();
  }
  before = db_->GetStats();
  go.store(true, std::memory_order_release);
  for (auto& th : threads) {
    th.join();
  }
  const DBStats after = db_->GetStats();
  EXPECT_EQ(after.super_version_slow_path, before.super_version_slow_path);
  EXPECT_EQ(after.super_version_fast_path - before.super_version_fast_path,
            static_cast<uint64_t>(kReaders) * kOps);
}

}  // namespace
}  // namespace talus
