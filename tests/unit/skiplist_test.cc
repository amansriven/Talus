#include "memtable/skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace talus {
namespace {

struct BytewiseComparator {
  int operator()(std::string_view a, std::string_view b) const { return a.compare(b); }
};

// Keys are big-endian uint64s, so bytewise order equals numeric order.
std::string EncodeKey(uint64_t v) {
  std::string s(8, '\0');
  for (int i = 0; i < 8; ++i) {
    s[i] = static_cast<char>(v >> (56 - 8 * i));
  }
  return s;
}

uint64_t DecodeKey(std::string_view s) {
  uint64_t v = 0;
  for (char c : s) {
    v = (v << 8) | static_cast<uint8_t>(c);
  }
  return v;
}

std::string ValueFor(uint64_t key) { return "v" + std::to_string(key); }

template <typename List>
void InsertKey(List* list, uint64_t key, bool concurrent) {
  const std::string k = EncodeKey(key);
  const std::string v = ValueFor(key);
  auto fill = [&](char* kd, char* vd) {
    std::memcpy(kd, k.data(), k.size());
    std::memcpy(vd, v.data(), v.size());
  };
  if (concurrent) {
    list->InsertConcurrently(k.size(), v.size(), fill);
  } else {
    list->Insert(k.size(), v.size(), fill);
  }
}

// Run the single-threaded tests with prefetching on and off, and with both
// insert paths.
template <bool kPrefetch>
struct PrefetchMode {
  static constexpr bool value = kPrefetch;
};
template <typename Mode>
class SkipListTest : public ::testing::Test {};
using Modes = ::testing::Types<PrefetchMode<true>, PrefetchMode<false>>;
TYPED_TEST_SUITE(SkipListTest, Modes);

TYPED_TEST(SkipListTest, Empty) {
  Arena arena;
  SkipList<BytewiseComparator, TypeParam::value> list(BytewiseComparator(), &arena);
  typename decltype(list)::Iterator iter(&list);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek(EncodeKey(100));
  EXPECT_FALSE(iter.Valid());
}

TYPED_TEST(SkipListTest, InsertSeekAndIterateMatchModel) {
  for (bool concurrent : {false, true}) {
    SCOPED_TRACE(concurrent ? "InsertConcurrently" : "Insert");
    Arena arena;
    SkipList<BytewiseComparator, TypeParam::value> list(BytewiseComparator(), &arena);
    std::set<uint64_t> model;
    std::mt19937_64 rng(301);
    constexpr uint64_t kRange = 5000;
    for (int i = 0; i < 2000; ++i) {
      const uint64_t key = rng() % kRange;
      if (model.insert(key).second) {
        InsertKey(&list, key, concurrent);
      }
    }

    // Full scan matches the model in order, with the right values.
    typename decltype(list)::Iterator iter(&list);
    iter.SeekToFirst();
    for (uint64_t expected : model) {
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(DecodeKey(iter.key()), expected);
      EXPECT_EQ(iter.value(), ValueFor(expected));
      iter.Next();
    }
    EXPECT_FALSE(iter.Valid());

    // Seek lands on the model's lower bound for every possible target.
    for (uint64_t target = 0; target <= kRange; ++target) {
      iter.Seek(EncodeKey(target));
      auto lb = model.lower_bound(target);
      if (lb == model.end()) {
        EXPECT_FALSE(iter.Valid()) << target;
      } else {
        ASSERT_TRUE(iter.Valid()) << target;
        EXPECT_EQ(DecodeKey(iter.key()), *lb) << target;
      }
    }
  }
}

TEST(SkipListHeightTest, RandomHeightHasBranchingFactorFour) {
  using List = SkipList<BytewiseComparator>;
  constexpr int kSamples = 1'000'000;
  int at_least[List::kMaxHeight + 2] = {};
  for (int i = 0; i < kSamples; ++i) {
    const int h = List::RandomHeight();
    ASSERT_GE(h, 1);
    ASSERT_LE(h, List::kMaxHeight);
    for (int level = 1; level <= h; ++level) {
      ++at_least[level];
    }
  }
  // P(height >= n) = 4^-(n-1). Check the first levels, where the expected
  // counts are large enough for a tight bound.
  double expected = kSamples;
  for (int level = 1; level <= 5; ++level) {
    EXPECT_NEAR(at_least[level], expected, expected * 0.05 + 50) << "level " << level;
    expected /= 4;
  }
}

// Writers insert disjoint key sets concurrently. Afterwards the list must hold
// exactly the union, in order.
TEST(SkipListConcurrentTest, ConcurrentInsertsAreAllPresentAndOrdered) {
  Arena arena;
  SkipList<BytewiseComparator> list(BytewiseComparator(), &arena);
  constexpr int kThreads = 8;
  constexpr uint64_t kPerThread = 20000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&list, t] {
      // Thread t owns keys congruent to t mod kThreads, inserted in random
      // order so threads contend at many positions.
      std::vector<uint64_t> keys;
      for (uint64_t i = 0; i < kPerThread; ++i) {
        keys.push_back(i * kThreads + t);
      }
      std::shuffle(keys.begin(), keys.end(), std::mt19937_64(static_cast<uint64_t>(t)));
      for (uint64_t key : keys) {
        InsertKey(&list, key, /*concurrent=*/true);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  SkipList<BytewiseComparator>::Iterator iter(&list);
  iter.SeekToFirst();
  for (uint64_t expected = 0; expected < kThreads * kPerThread; ++expected) {
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(DecodeKey(iter.key()), expected);
    ASSERT_EQ(iter.value(), ValueFor(expected));
    iter.Next();
  }
  EXPECT_FALSE(iter.Valid());
}

// Readers scan while writers insert. Writer w inserts keys (w, 0), (w, 1), ...
// in order and publishes its progress with a release store after each one. A
// reader that acquires progress p before scanning must see at least
// (w, 0) .. (w, p-1). Because each writer inserts in order, whatever prefix of
// w's keys the reader sees must be gap-free. Every scan must also be strictly
// ascending.
void RunReadersDuringWrites(bool concurrent_writers) {
  Arena arena;
  SkipList<BytewiseComparator> list(BytewiseComparator(), &arena);
  const int writers = concurrent_writers ? 4 : 1;
  constexpr int kReaders = 3;
  constexpr uint64_t kPerWriter = 10000;
  constexpr uint64_t kWriterShift = 32;

  std::vector<std::atomic<uint64_t>> progress(writers);
  std::atomic<int> writers_done{0};
  std::atomic<int> readers_ready{0};

  std::vector<std::thread> threads;
  for (int w = 0; w < writers; ++w) {
    threads.emplace_back([&, w] {
      // Hold writers back until every reader is scanning, so they overlap.
      // Relaxed: a start signal only; it orders no data.
      while (readers_ready.load(std::memory_order_relaxed) < kReaders) {
        std::this_thread::yield();
      }
      for (uint64_t i = 0; i < kPerWriter; ++i) {
        InsertKey(&list, (static_cast<uint64_t>(w) << kWriterShift) | i, concurrent_writers);
        // Release: pairs with the reader's acquire, ordering this insert before the reader's scan.
        progress[w].store(i + 1, std::memory_order_release);
      }
      // Release/acquire not needed: only used as a loop-exit signal.
      writers_done.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&] {
      std::vector<uint64_t> snapshot(writers);
      std::vector<uint64_t> seen(writers);
      readers_ready.fetch_add(1, std::memory_order_relaxed);
      bool last_pass = false;
      while (!last_pass) {
        last_pass = writers_done.load(std::memory_order_relaxed) == writers;
        for (int w = 0; w < writers; ++w) {
          // Acquire: see the inserts that happened before this progress value.
          snapshot[w] = progress[w].load(std::memory_order_acquire);
          seen[w] = 0;
        }
        SkipList<BytewiseComparator>::Iterator iter(&list);
        uint64_t prev = 0;
        bool first = true;
        for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
          const uint64_t key = DecodeKey(iter.key());
          ASSERT_TRUE(first || key > prev) << "scan out of order";
          first = false;
          prev = key;
          const auto w = static_cast<int>(key >> kWriterShift);
          const uint64_t i = key & ((uint64_t{1} << kWriterShift) - 1);
          ASSERT_EQ(i, seen[w]) << "gap in writer " << w << "'s keys";
          ++seen[w];
        }
        for (int w = 0; w < writers; ++w) {
          ASSERT_GE(seen[w], snapshot[w]) << "missing published keys of writer " << w;
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

TEST(SkipListConcurrentTest, ReadersDuringSingleWriter) { RunReadersDuringWrites(false); }

TEST(SkipListConcurrentTest, ReadersDuringConcurrentWriters) { RunReadersDuringWrites(true); }

}  // namespace
}  // namespace talus
