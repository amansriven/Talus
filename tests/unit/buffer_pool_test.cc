#include "util/buffer_pool.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "util/port.h"

namespace talus {
namespace {

TEST(BufferPoolTest, AllocationsAreUsableAndDistinct) {
  HugePageBufferPool pool;
  std::vector<std::pair<char*, size_t>> live;
  std::mt19937 rng(301);
  for (int i = 0; i < 2000; ++i) {
    // Mostly block-sized, some tiny, some above the pooled limit.
    const size_t n = i % 50 == 0 ? 70000 + rng() % 1000 : 1 + rng() % 9000;
    char* p = pool.Allocate(n);
    std::memset(p, i & 0xff, n);
    live.emplace_back(p, n);
  }
  for (size_t i = 0; i < live.size(); ++i) {
    const auto [p, n] = live[i];
    for (size_t b = 0; b < n; b += 97) {
      ASSERT_EQ(static_cast<unsigned char>(p[b]), i & 0xff);
    }
    pool.Free(p, n);
  }
  EXPECT_GT(pool.ChunkBytes(), 0u);
  EXPECT_EQ(pool.ChunkBytes() % kHugePageSize, 0u);
}

TEST(BufferPoolTest, FreedBuffersAreReusedBySizeClass) {
  HugePageBufferPool pool;
  char* a = pool.Allocate(4100);  // Class 4608.
  pool.Free(a, 4100);
  EXPECT_EQ(pool.Allocate(4500), a);  // Same class: the freed buffer comes back.
  pool.Free(a, 4500);
  const size_t chunk_bytes = pool.ChunkBytes();
  for (int i = 0; i < 1000; ++i) {  // Steady-state churn needs no new chunks.
    char* p = pool.Allocate(4096);
    pool.Free(p, 4096);
  }
  EXPECT_EQ(pool.ChunkBytes(), chunk_bytes);
}

TEST(BufferPoolTest, LargeRequestsBypassChunks) {
  HugePageBufferPool pool;
  char* p = pool.Allocate(HugePageBufferPool::kMaxPooledSize + 1);
  EXPECT_EQ(pool.ChunkBytes(), 0u);
  pool.Free(p, HugePageBufferPool::kMaxPooledSize + 1);
}

TEST(BufferPoolTest, BufferReturnsToItsPool) {
  HugePageBufferPool pool;
  char* first = nullptr;
  {
    Buffer b = AllocateBuffer(1000, &pool);
    first = b.get();
  }
  Buffer again = AllocateBuffer(1000, &pool);
  EXPECT_EQ(again.get(), first);
  Buffer heap = AllocateBuffer(1000, nullptr);  // Plain heap buffer.
  EXPECT_NE(heap.get(), nullptr);
}

TEST(BufferPoolTest, ConcurrentAllocateAndFree) {
  HugePageBufferPool pool;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&pool, t] {
      std::mt19937 rng(static_cast<uint32_t>(t));
      std::vector<std::pair<char*, size_t>> mine;
      for (int i = 0; i < 20000; ++i) {
        if (mine.size() < 50 && rng() % 3 != 0) {
          const size_t n = 1 + rng() % 8192;
          char* p = pool.Allocate(n);
          std::memset(p, t, n);
          mine.emplace_back(p, n);
        } else if (!mine.empty()) {
          const auto [p, n] = mine.back();
          mine.pop_back();
          ASSERT_EQ(p[n - 1], static_cast<char>(t));  // Nobody else touched it.
          pool.Free(p, n);
        }
      }
      for (const auto& [p, n] : mine) {
        pool.Free(p, n);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

}  // namespace
}  // namespace talus
