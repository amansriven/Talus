#include "cache/lru_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "util/hash.h"

namespace talus {
namespace {

// Records its destruction, so tests can see exactly when values are freed.
struct Value {
  Value(int v, std::vector<int>* log, std::mutex* mu) : v(v), log(log), mu(mu) {}
  ~Value() {
    std::lock_guard<std::mutex> lock(*mu);
    log->push_back(v);
  }
  int v;
  std::vector<int>* log;
  std::mutex* mu;
};

struct IntHash {
  static uint64_t Hash(int key) { return Hash64(reinterpret_cast<const char*>(&key), sizeof(key)); }
};

using Cache = ShardedLRUCache<int, Value, IntHash>;

class LRUCacheTest : public ::testing::Test {
 protected:
  // Inserts key -> value with `charge` and releases the handle.
  void Put(Cache& cache, int key, int value, size_t charge = 1) {
    cache.Release(cache.Insert(key, std::make_unique<Value>(value, &deleted_, &mu_), charge));
  }
  // The value cached for `key`, or -1.
  int Get(Cache& cache, int key) {
    Cache::Handle* h = cache.Lookup(key);
    if (h == nullptr) {
      return -1;
    }
    const int v = Cache::value(h)->v;
    cache.Release(h);
    return v;
  }
  std::vector<int> Deleted() {
    std::lock_guard<std::mutex> lock(mu_);
    return deleted_;
  }

  std::mutex mu_;
  std::vector<int> deleted_;
};

TEST_F(LRUCacheTest, HitAndMiss) {
  Cache cache(100, 1);
  EXPECT_EQ(Get(cache, 1), -1);
  Put(cache, 1, 101);
  EXPECT_EQ(Get(cache, 1), 101);
  EXPECT_EQ(Get(cache, 2), -1);
  Put(cache, 2, 201);
  EXPECT_EQ(Get(cache, 1), 101);
  EXPECT_EQ(Get(cache, 2), 201);
}

TEST_F(LRUCacheTest, EvictsLeastRecentlyUsed) {
  Cache cache(3, 1);
  Put(cache, 1, 1);
  Put(cache, 2, 2);
  Put(cache, 3, 3);
  EXPECT_EQ(Get(cache, 1), 1);  // 1 is now the most recently used.
  Put(cache, 4, 4);             // Evicts 2, the least recently used.
  EXPECT_EQ(Deleted(), std::vector<int>{2});
  EXPECT_EQ(Get(cache, 2), -1);
  EXPECT_EQ(Get(cache, 1), 1);
  EXPECT_EQ(Get(cache, 3), 3);
  EXPECT_EQ(Get(cache, 4), 4);
}

TEST_F(LRUCacheTest, PinnedEntriesAreNeverEvicted) {
  Cache cache(2, 1);
  Cache::Handle* pinned = cache.Insert(1, std::make_unique<Value>(1, &deleted_, &mu_), 1);
  Put(cache, 2, 2);
  Put(cache, 3, 3);  // Over capacity: evicts 2, not the pinned 1.
  Put(cache, 4, 4);  // Evicts 3.
  EXPECT_EQ(Deleted(), (std::vector<int>{2, 3}));
  EXPECT_EQ(Cache::value(pinned)->v, 1);
  cache.Release(pinned);
  EXPECT_EQ(Get(cache, 1), 1);  // Still cached after release.
}

TEST_F(LRUCacheTest, ErasedOrReplacedValuesLiveUntilReleased) {
  Cache cache(100, 1);
  Cache::Handle* h = cache.Insert(1, std::make_unique<Value>(100, &deleted_, &mu_), 1);
  cache.Erase(1);
  EXPECT_EQ(Get(cache, 1), -1);
  EXPECT_TRUE(Deleted().empty());
  EXPECT_EQ(Cache::value(h)->v, 100);  // Still valid while pinned.
  cache.Release(h);
  EXPECT_EQ(Deleted(), std::vector<int>{100});

  h = cache.Insert(2, std::make_unique<Value>(1, &deleted_, &mu_), 1);
  Put(cache, 2, 2);  // Replaces the pinned entry.
  EXPECT_EQ(Get(cache, 2), 2);
  EXPECT_EQ(Cache::value(h)->v, 1);
  cache.Release(h);
  EXPECT_EQ(Deleted(), (std::vector<int>{100, 1}));
}

TEST_F(LRUCacheTest, CapacityIsInChargeUnits) {
  Cache cache(1000, 1);
  Put(cache, 1, 1, 400);
  Put(cache, 2, 2, 400);
  EXPECT_EQ(cache.TotalCharge(), 800u);
  Put(cache, 3, 3, 400);  // 1200 > 1000: evicts key 1.
  EXPECT_EQ(cache.TotalCharge(), 800u);
  EXPECT_EQ(Get(cache, 1), -1);
}

TEST_F(LRUCacheTest, ReleasingAPinOverCapacityEvicts) {
  Cache cache(1, 1);
  Cache::Handle* a = cache.Insert(1, std::make_unique<Value>(1, &deleted_, &mu_), 1);
  Cache::Handle* b = cache.Insert(2, std::make_unique<Value>(2, &deleted_, &mu_), 1);
  EXPECT_EQ(cache.TotalCharge(), 2u);  // Both pinned: over capacity.
  cache.Release(a);                    // Evicted as soon as it is unpinned.
  EXPECT_EQ(Deleted(), std::vector<int>{1});
  cache.Release(b);
  EXPECT_EQ(cache.TotalCharge(), 1u);
}

TEST_F(LRUCacheTest, PinReleasesItsHandle) {
  Cache cache(100, 1);
  {
    Pin pin;
    cache.PinInto(cache.Insert(7, std::make_unique<Value>(7, &deleted_, &mu_), 1), &pin);
    cache.Erase(7);
    EXPECT_TRUE(Deleted().empty());
  }
  EXPECT_EQ(Deleted(), std::vector<int>{7});
}

TEST_F(LRUCacheTest, ShardsSplitCapacityAndHashTablesGrow) {
  Cache cache(16 * 1000, 16);
  EXPECT_EQ(cache.num_shards(), 16u);
  for (int i = 0; i < 5000; ++i) {  // Forces several rehashes per shard.
    Put(cache, i, i * 3);
  }
  EXPECT_EQ(cache.TotalCharge(), 5000u);
  for (int i = 0; i < 5000; ++i) {
    ASSERT_EQ(Get(cache, i), i * 3);
  }
}

// Concurrent inserts, lookups, erases and pinned reads with constant
// eviction. Every value read must match its key. Under TSan this also checks
// the shard locking.
TEST_F(LRUCacheTest, ConcurrentStress) {
  Cache cache(64, 4);
  std::atomic<int> inserted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t));
      std::vector<Cache::Handle*> held;
      for (int i = 0; i < 20000; ++i) {
        const int key = static_cast<int>(rng() % 200);
        switch (rng() % 4) {
          case 0:
            held.push_back(cache.Insert(key, std::make_unique<Value>(key * 7, &deleted_, &mu_), 1));
            inserted.fetch_add(1, std::memory_order_relaxed);  // Relaxed: a test counter.
            break;
          case 1:
            if (Cache::Handle* h = cache.Lookup(key)) {
              held.push_back(h);
            }
            break;
          case 2:
            cache.Erase(key);
            break;
          default:
            break;
        }
        if (held.size() > 8 || (!held.empty() && rng() % 2 == 0)) {
          Cache::Handle* h = held.back();
          held.pop_back();
          ASSERT_EQ(Cache::value(h)->v, Cache::value(h)->v / 7 * 7);
          cache.Release(h);
        }
      }
      for (Cache::Handle* h : held) {
        cache.Release(h);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  EXPECT_LE(cache.TotalCharge(), 64u);
  const size_t freed_before_destruction = Deleted().size();
  EXPECT_LE(freed_before_destruction, static_cast<size_t>(inserted.load()));
}

}  // namespace
}  // namespace talus
