#include "memtable/memtable.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace talus {
namespace {

// The result of a memtable lookup as a comparable string.
std::string Lookup(const MemTable& mem, std::string_view key, SequenceNumber seq) {
  LookupKey lkey(key, seq);
  std::string_view value;
  Status s;
  if (!mem.Get(lkey, &value, &s)) {
    return "<absent>";
  }
  if (s.IsNotFound()) {
    return "<deleted>";
  }
  EXPECT_TRUE(s.ok());
  return std::string(value);
}

class MemTableTest : public ::testing::TestWithParam<bool> {
 protected:
  MemTable mem_{/*concurrent_writes=*/GetParam()};
};

INSTANTIATE_TEST_SUITE_P(WriteModes, MemTableTest, ::testing::Bool(), [](const auto& info) {
  return info.param ? "Concurrent" : "SingleWriter";
});

TEST_P(MemTableTest, Empty) {
  EXPECT_EQ(Lookup(mem_, "foo", kMaxSequenceNumber), "<absent>");
  MemTable::Iterator iter = mem_.NewIterator();
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
}

TEST_P(MemTableTest, ReadsSeeNewestVersionAtOrBelowSnapshot) {
  mem_.Add(10, ValueType::kValue, "k", "v10");
  mem_.Add(20, ValueType::kValue, "k", "v20");
  mem_.Add(30, ValueType::kDeletion, "k", "");
  mem_.Add(40, ValueType::kValue, "k", "v40");

  EXPECT_EQ(Lookup(mem_, "k", 9), "<absent>");
  EXPECT_EQ(Lookup(mem_, "k", 10), "v10");
  EXPECT_EQ(Lookup(mem_, "k", 19), "v10");
  EXPECT_EQ(Lookup(mem_, "k", 20), "v20");
  EXPECT_EQ(Lookup(mem_, "k", 30), "<deleted>");
  EXPECT_EQ(Lookup(mem_, "k", 39), "<deleted>");
  EXPECT_EQ(Lookup(mem_, "k", 40), "v40");
  EXPECT_EQ(Lookup(mem_, "k", kMaxSequenceNumber), "v40");
}

TEST_P(MemTableTest, DistinguishesKeysSharingPrefixes) {
  mem_.Add(1, ValueType::kValue, "", "empty-key");
  mem_.Add(2, ValueType::kValue, "a", "a");
  mem_.Add(3, ValueType::kValue, "ab", "ab");
  mem_.Add(4, ValueType::kValue, "abc", "");

  EXPECT_EQ(Lookup(mem_, "", 10), "empty-key");
  EXPECT_EQ(Lookup(mem_, "a", 10), "a");
  EXPECT_EQ(Lookup(mem_, "ab", 10), "ab");
  EXPECT_EQ(Lookup(mem_, "abc", 10), "");
  EXPECT_EQ(Lookup(mem_, "aa", 10), "<absent>");
  EXPECT_EQ(Lookup(mem_, "abcd", 10), "<absent>");
  EXPECT_EQ(Lookup(mem_, "b", 10), "<absent>");
}

TEST_P(MemTableTest, LargeKeysAndValues) {
  const std::string key(1000, 'k');      // Longer than LookupKey's inline buffer.
  const std::string value(100000, 'v');  // Takes the arena's dedicated-block path.
  mem_.Add(7, ValueType::kValue, key, value);
  EXPECT_EQ(Lookup(mem_, key, 7), value);
  EXPECT_GE(mem_.ApproximateMemoryUsage(), key.size() + value.size());
}

TEST_P(MemTableTest, IteratorYieldsInternalKeyOrder) {
  mem_.Add(1, ValueType::kValue, "b", "b1");
  mem_.Add(2, ValueType::kValue, "a", "a2");
  mem_.Add(3, ValueType::kDeletion, "b", "");
  mem_.Add(4, ValueType::kValue, "a", "a4");

  struct Expected {
    std::string_view user_key;
    SequenceNumber seq;
    ValueType type;
    std::string_view value;
  };
  const Expected expected[] = {
      {"a", 4, ValueType::kValue, "a4"},
      {"a", 2, ValueType::kValue, "a2"},
      {"b", 3, ValueType::kDeletion, ""},
      {"b", 1, ValueType::kValue, "b1"},
  };
  MemTable::Iterator iter = mem_.NewIterator();
  iter.SeekToFirst();
  for (const Expected& e : expected) {
    ASSERT_TRUE(iter.Valid());
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(iter.key(), &parsed));
    EXPECT_EQ(parsed.user_key, e.user_key);
    EXPECT_EQ(parsed.sequence, e.seq);
    EXPECT_EQ(parsed.type, e.type);
    EXPECT_EQ(iter.value(), e.value);
    iter.Next();
  }
  EXPECT_FALSE(iter.Valid());
}

TEST(MemTableConcurrentTest, ConcurrentAddsAreAllVisible) {
  MemTable mem(/*concurrent_writes=*/true);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 10000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&mem, t] {
      for (int i = 0; i < kPerThread; ++i) {
        // Every thread writes every key; sequences are globally unique.
        const auto seq = static_cast<SequenceNumber>(i) * kThreads + t + 1;
        mem.Add(seq, ValueType::kValue, "key" + std::to_string(i), std::to_string(seq));
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  for (int i = 0; i < kPerThread; ++i) {
    const std::string key = "key" + std::to_string(i);
    // The newest version was written by the last thread.
    const SequenceNumber newest = static_cast<SequenceNumber>(i) * kThreads + kThreads;
    ASSERT_EQ(Lookup(mem, key, kMaxSequenceNumber), std::to_string(newest));
    // At each thread's sequence, that thread's version is the visible one.
    for (int t = 0; t < kThreads; ++t) {
      const auto seq = static_cast<SequenceNumber>(i) * kThreads + t + 1;
      ASSERT_EQ(Lookup(mem, key, seq), std::to_string(seq));
    }
  }
}

}  // namespace
}  // namespace talus
