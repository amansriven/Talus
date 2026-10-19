#include "util/arena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace talus {
namespace {

bool IsAligned(const char* p) { return reinterpret_cast<uintptr_t>(p) % Arena::kAlignment == 0; }

TEST(ArenaTest, Empty) {
  Arena arena;
  EXPECT_GT(arena.MemoryUsage(), 0u);  // The first block is allocated eagerly.
}

// Single-threaded: every allocation is aligned, writable, and keeps its
// contents, across many small, medium and dedicated (large) allocations.
TEST(ArenaTest, Simple) {
  std::vector<std::pair<size_t, char*>> allocated;
  Arena arena;
  std::mt19937 rng(301);
  size_t bytes = 0;
  constexpr int kN = 100000;
  for (int i = 0; i < kN; ++i) {
    size_t s;
    if (i % (kN / 10) == 0) {
      s = i == 0 ? 1 : static_cast<size_t>(i);
    } else {
      s = rng() % 4000 == 0 ? rng() % 6000 + 1
                            : (rng() % 10 == 0 ? rng() % 100 + 1 : rng() % 20 + 1);
    }
    char* r = arena.Allocate(s);
    ASSERT_TRUE(IsAligned(r));
    for (size_t b = 0; b < s; ++b) {
      r[b] = static_cast<char>(i % 256);
    }
    bytes += (s + Arena::kAlignment - 1) / Arena::kAlignment * Arena::kAlignment;
    allocated.emplace_back(s, r);
    ASSERT_GE(arena.MemoryUsage(), bytes);
    if (i > kN / 10) {
      ASSERT_LE(arena.MemoryUsage(), bytes * 1.25);
    }
  }
  for (size_t i = 0; i < allocated.size(); ++i) {
    const auto [size, p] = allocated[i];
    for (size_t b = 0; b < size; ++b) {
      ASSERT_EQ(p[b] & 0xff, static_cast<int>(i % 256));
    }
  }
}

TEST(ArenaTest, HugePagesOption) {
  Arena arena(Arena::kDefaultBlockSize, /*use_huge_pages=*/true);
  EXPECT_GE(arena.MemoryUsage(), kHugePageSize);
  char* p = arena.Allocate(1000);
  ASSERT_TRUE(IsAligned(p));
  std::memset(p, 0xab, 1000);
}

// Many threads allocate concurrently and stamp each allocation with a pattern
// unique to (thread, allocation). Afterwards every pattern must be intact: any
// overlap between two handed-out ranges would corrupt one of them. Run under
// TSan this also checks the arena's memory ordering.
TEST(ArenaTest, ConcurrentAllocationsDoNotOverlap) {
  Arena arena;
  constexpr int kThreads = 8;
  constexpr int kAllocsPerThread = 20000;

  struct Alloc {
    char* ptr;
    size_t size;
  };
  std::vector<std::vector<Alloc>> per_thread(kThreads);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&arena, &per_thread, t] {
      std::mt19937 rng(static_cast<uint32_t>(t) + 1);
      auto& mine = per_thread[t];
      mine.reserve(kAllocsPerThread);
      for (int i = 0; i < kAllocsPerThread; ++i) {
        // Mostly small, occasionally large enough to hit the dedicated path.
        const size_t size = rng() % 500 == 0 ? 2000 + rng() % 2000 : 1 + rng() % 128;
        char* p = arena.Allocate(size);
        ASSERT_TRUE(IsAligned(p));
        std::memset(p, (t * 31 + i) & 0xff, size);
        mine.push_back({p, size});
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  size_t total = 0;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kAllocsPerThread; ++i) {
      const Alloc& a = per_thread[t][i];
      const auto expected = static_cast<char>((t * 31 + i) & 0xff);
      for (size_t b = 0; b < a.size; ++b) {
        ASSERT_EQ(a.ptr[b], expected) << "thread " << t << " alloc " << i;
      }
      total += a.size;
    }
  }
  EXPECT_GE(arena.MemoryUsage(), total);
}

}  // namespace
}  // namespace talus
