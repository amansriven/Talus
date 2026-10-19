#include "table/block.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "table/block_builder.h"

namespace talus {
namespace {

struct Bytewise {
  int operator()(std::string_view a, std::string_view b) const { return a.compare(b); }
};

using KVs = std::vector<std::pair<std::string, std::string>>;

Block MakeBlock(std::string_view bytes) {
  auto heap = std::make_unique<char[]>(bytes.size());
  std::memcpy(heap.get(), bytes.data(), bytes.size());
  BlockContents contents;
  contents.data = std::string_view(heap.get(), bytes.size());
  contents.heap = std::move(heap);
  return Block(std::move(contents));
}

std::string BuildBytes(const KVs& kvs, int restart_interval) {
  BlockBuilder builder(restart_interval);
  for (const auto& [k, v] : kvs) {
    builder.Add(k, v);
  }
  return std::string(builder.Finish());
}

KVs SampleEntries(size_t n, size_t key_padding = 0) {
  KVs kvs;
  std::mt19937 rng(301);
  for (size_t i = 0; i < n; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key%06zu", i * 3);  // Gaps for in-between seeks.
    kvs.emplace_back(std::string(key) + std::string(key_padding, 'p'),
                     std::string(rng() % 50, static_cast<char>('a' + i % 26)));
  }
  return kvs;
}

TEST(BlockTest, EmptyBlock) {
  Block block = MakeBlock(BuildBytes({}, 16));
  ASSERT_TRUE(block.well_formed());
  BlockIter<Bytewise> iter(block);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek("anything");
  EXPECT_FALSE(iter.Valid());
  EXPECT_TRUE(iter.status().ok());
}

class BlockRoundTripTest : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(RestartIntervals, BlockRoundTripTest, ::testing::Values(1, 2, 16, 1000));

TEST_P(BlockRoundTripTest, IterateAndSeek) {
  for (size_t padding : {size_t{0}, size_t{300}}) {  // 300: keys spill KeyBuffer to the heap.
    const KVs kvs = SampleEntries(500, padding);
    Block block = MakeBlock(BuildBytes(kvs, GetParam()));
    ASSERT_TRUE(block.well_formed());
    BlockIter<Bytewise> iter(block);

    iter.SeekToFirst();
    for (const auto& [k, v] : kvs) {
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(iter.key(), k);
      EXPECT_EQ(iter.value(), v);
      iter.Next();
    }
    EXPECT_FALSE(iter.Valid());
    EXPECT_TRUE(iter.status().ok());

    for (size_t i = 0; i < kvs.size(); ++i) {
      iter.Seek(kvs[i].first);  // Exact match.
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(iter.key(), kvs[i].first);
      // Just past key i lands on key i + 1.
      iter.Seek(kvs[i].first + '\0');
      if (i + 1 < kvs.size()) {
        ASSERT_TRUE(iter.Valid());
        EXPECT_EQ(iter.key(), kvs[i + 1].first);
      } else {
        EXPECT_FALSE(iter.Valid());
      }
    }
    iter.Seek("");
    ASSERT_TRUE(iter.Valid());
    EXPECT_EQ(iter.key(), kvs.front().first);
    iter.Seek("zzz");
    EXPECT_FALSE(iter.Valid());
  }
}

TEST(BlockTest, PrefixCompressionShrinksBlock) {
  // Keys share a 40-byte prefix; with restarts every 16 entries most entries
  // store only their few distinct trailing bytes.
  KVs kvs;
  for (int i = 0; i < 200; ++i) {
    kvs.emplace_back(std::string(40, 'p') + std::to_string(1000 + i), "");
  }
  const size_t compressed = BuildBytes(kvs, 16).size();
  const size_t uncompressed = BuildBytes(kvs, 1).size();
  EXPECT_LT(compressed, uncompressed / 3) << compressed << " vs " << uncompressed;
}

TEST(BlockTest, BadRestartCountIsMalformed) {
  std::string bytes = BuildBytes(SampleEntries(10), 4);
  EncodeFixed32(bytes.data() + bytes.size() - 4, 1'000'000);
  Block block = MakeBlock(bytes);
  EXPECT_FALSE(block.well_formed());
  BlockIter<Bytewise> iter(block);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  EXPECT_TRUE(iter.status().IsCorruption());
  EXPECT_FALSE(MakeBlock("ab").well_formed());
}

TEST(BlockTest, RestartEntryWithSharedPrefixIsCorruption) {
  // One entry claiming 3 shared bytes, but it is a restart point.
  std::string bytes;
  PutVarint32(&bytes, 3);
  PutVarint32(&bytes, 1);
  PutVarint32(&bytes, 0);
  bytes += "k";
  PutFixed32(&bytes, 0);
  PutFixed32(&bytes, 1);
  Block block = MakeBlock(bytes);
  BlockIter<Bytewise> iter(block);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  EXPECT_TRUE(iter.status().IsCorruption());
}

TEST(BlockTest, InternalKeysShorterThanTagAreCorruption) {
  Block block = MakeBlock(BuildBytes({{"short", "v"}}, 16));
  BlockIter<InternalKeyComparator> iter(block);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  EXPECT_TRUE(iter.status().IsCorruption());
}

// Random byte flips must never crash or read outside the block (checked by
// running under ASan). Detecting the corruption is the block CRC's job
// (table/format.cc); the parser only has to stay in bounds.
TEST(BlockTest, RandomCorruptionNeverReadsOutOfBounds) {
  const std::string good = BuildBytes(SampleEntries(100), 8);
  std::mt19937 rng(7);
  for (int trial = 0; trial < 2000; ++trial) {
    std::string bad = good;
    const int flips = 1 + static_cast<int>(rng() % 4);
    for (int f = 0; f < flips; ++f) {
      bad[rng() % bad.size()] = static_cast<char>(rng());
    }
    // Copy into an exactly-sized heap buffer, so ASan flags any overrun.
    Block block = MakeBlock(bad);
    BlockIter<Bytewise> iter(block);
    size_t steps = 0;
    for (iter.SeekToFirst(); iter.Valid() && steps < 1000; iter.Next(), ++steps) {
      volatile char sink = iter.key().empty() ? 0 : iter.key().back();
      sink = iter.value().empty() ? 0 : iter.value().back();
      (void)sink;
    }
    iter.Seek("key000150");
  }
}

}  // namespace
}  // namespace talus
