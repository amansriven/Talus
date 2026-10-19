#include "cache/table_cache.h"

#include <dirent.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "table/table_builder.h"
#include "test_util.h"
#include "util/filename.h"

namespace talus {
namespace {

constexpr int kKeysPerTable = 2000;

std::string UserKey(uint64_t file, int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "f%02d_k%05d", static_cast<int>(file), i);
  return buf;
}

std::string ValueFor(uint64_t file, int i) {
  return std::string(100, static_cast<char>('a' + (file + static_cast<uint64_t>(i)) % 26)) +
         std::to_string(i);
}

// Number of open file descriptors in this process.
int OpenFds() {
  int n = 0;
  if (DIR* d = ::opendir("/dev/fd")) {
    while (::readdir(d) != nullptr) {
      ++n;
    }
    ::closedir(d);
  }
  return n;
}

class TableCacheTest : public ::testing::Test {
 protected:
  // Writes table `file` holding UserKey(file, i) -> ValueFor(file, i).
  uint64_t BuildTable(uint64_t file) {
    std::unique_ptr<WritableFile> out;
    EXPECT_TRUE(WritableFile::Create(TableFileName(dir_.path(), file), &out).ok());
    TableBuilder builder(TableOptions{}, out.get());
    for (int i = 0; i < kKeysPerTable; ++i) {
      std::string ikey;
      AppendInternalKey(&ikey, UserKey(file, i), 1, ValueType::kValue);
      builder.Add(ikey, ValueFor(file, i));
    }
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(out->Close().ok());
    sizes_.resize(std::max<size_t>(sizes_.size(), file + 1));
    sizes_[file] = builder.FileSize();
    return builder.FileSize();
  }

  // Looks up key i of `file`. Returns the value ("" if absent) and, if
  // `keep` is non-null, moves the value's pin there.
  std::string Lookup(TableCache& cache, uint64_t file, int i, const TableReadOptions& options = {},
                     Pin* keep = nullptr, std::string_view* view = nullptr) {
    const LookupKey lkey(UserKey(file, i), kMaxSequenceNumber);
    std::string result;
    Status s = cache.Get(file, sizes_[file], lkey.internal_key(), options,
                         [&](std::string_view key, std::string_view value, Pin& pin) {
                           if (ExtractUserKey(key) == UserKey(file, i)) {
                             result = value;
                             if (keep != nullptr) {
                               *keep = std::move(pin);
                               *view = value;
                             }
                           }
                         });
    EXPECT_TRUE(s.ok()) << s.ToString();
    return result;
  }

  test::ScopedTempDir dir_;
  std::vector<uint64_t> sizes_;
};

TEST_F(TableCacheTest, SecondReadOfABlockIsACacheHit) {
  BuildTable(1);
  BlockCache block_cache(8 << 20, 16);
  TableCache cache(dir_.path(), {.block_cache = &block_cache});

  TableReadStats stats;
  const TableReadOptions options{.stats = &stats};
  EXPECT_EQ(Lookup(cache, 1, 42, options), ValueFor(1, 42));
  EXPECT_EQ(stats.block_cache_misses, 1u);
  EXPECT_EQ(stats.block_cache_hits, 0u);
  EXPECT_EQ(stats.filter_checks, 1u);
  EXPECT_EQ(Lookup(cache, 1, 42, options), ValueFor(1, 42));
  EXPECT_EQ(stats.block_cache_hits, 1u);
  EXPECT_GT(block_cache.TotalCharge(), 0u);

  // An absent key is usually ruled out by the filter without reading a block.
  const LookupKey absent("f01_absent", kMaxSequenceNumber);
  ASSERT_TRUE(cache
                  .Get(1, sizes_[1], absent.internal_key(), options,
                       [](std::string_view, std::string_view, Pin&) {})
                  .ok());
  EXPECT_EQ(stats.filter_checks, 3u);
}

TEST_F(TableCacheTest, FillCacheFalseDoesNotPopulateTheCache) {
  BuildTable(1);
  BlockCache block_cache(8 << 20, 16);
  TableCache cache(dir_.path(), {.block_cache = &block_cache});
  TableReadStats stats;
  const TableReadOptions no_fill{.fill_cache = false, .stats = &stats};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(Lookup(cache, 1, 7, no_fill), ValueFor(1, 7));
  }
  EXPECT_EQ(stats.block_cache_misses, 3u);
  EXPECT_EQ(block_cache.TotalCharge(), 0u);
}

// A value held through its pin must stay valid after its block is evicted
// and its table is closed. Under ASan, a use-after-free would fail here.
TEST_F(TableCacheTest, PinnedValueOutlivesEvictionAndTableClose) {
  BuildTable(1);
  BuildTable(2);
  BlockCache block_cache(1, 1);  // Everything unpinned is evicted at once.
  TableCache cache(dir_.path(), {.block_cache = &block_cache, .max_open_files = 1});

  Pin pin;
  std::string_view value;
  ASSERT_EQ(Lookup(cache, 1, 1234, {}, &pin, &value), ValueFor(1, 1234));
  for (int i = 0; i < 200; ++i) {  // Churn both caches.
    ASSERT_EQ(Lookup(cache, 2, i * 7, {}), ValueFor(2, i * 7));
  }
  cache.Evict(1);
  EXPECT_EQ(value, ValueFor(1, 1234));
  pin.Reset();
}

TEST_F(TableCacheTest, MmapReadsBypassTheBlockCache) {
  BuildTable(1);
  BlockCache block_cache(8 << 20, 16);
  TableCache cache(dir_.path(),
                   {.read_mode = RandomAccessFile::Mode::kMmap, .block_cache = &block_cache});
  Pin pin;
  std::string_view value;
  EXPECT_EQ(Lookup(cache, 1, 99, {}, &pin, &value), ValueFor(1, 99));
  EXPECT_EQ(block_cache.TotalCharge(), 0u);
  cache.Evict(1);  // The pin keeps the mapping alive.
  EXPECT_EQ(value, ValueFor(1, 99));
}

TEST_F(TableCacheTest, HugePagePoolBacksCachedBlocks) {
  BuildTable(1);
  HugePageBufferPool pool;
  {
    BlockCache block_cache(8 << 20, 16);
    TableCache cache(dir_.path(), {.block_cache = &block_cache, .buffer_pool = &pool});
    for (int i = 0; i < kKeysPerTable; i += 50) {
      ASSERT_EQ(Lookup(cache, 1, i), ValueFor(1, i));
    }
    EXPECT_GT(pool.ChunkBytes(), 0u);
  }  // Caches release every block before the pool is destroyed.
}

TEST_F(TableCacheTest, OpenFilesStayWithinMaxOpenFiles) {
  constexpr uint64_t kTables = 12;
  for (uint64_t f = 1; f <= kTables; ++f) {
    BuildTable(f);
  }
  const int baseline = OpenFds();
  {
    TableCache cache(dir_.path(), {.max_open_files = 3});
    for (int round = 0; round < 3; ++round) {
      for (uint64_t f = 1; f <= kTables; ++f) {
        ASSERT_EQ(Lookup(cache, f, round * 100), ValueFor(f, round * 100));
        EXPECT_LE(OpenFds(), baseline + 3);
      }
    }
  }
  EXPECT_EQ(OpenFds(), baseline);  // Destroying the cache closes everything.
}

TEST_F(TableCacheTest, IteratorKeepsTableOpenAfterEvict) {
  BuildTable(3);
  TableCache cache(dir_.path(), {.max_open_files = 1});
  auto iter = cache.NewIterator(3, sizes_[3]);
  cache.Evict(3);
  int n = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++n) {
    ASSERT_EQ(iter->value(), ValueFor(3, n));
  }
  EXPECT_EQ(n, kKeysPerTable);
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(TableCacheTest, MissingOrWrongSizedFilesFail) {
  const uint64_t size = BuildTable(1);
  TableCache cache(dir_.path(), {});
  const LookupKey lkey(UserKey(1, 0), kMaxSequenceNumber);
  auto noop = [](std::string_view, std::string_view, Pin&) {};
  EXPECT_TRUE(cache.Get(1, size + 1, lkey.internal_key(), {}, noop).IsCorruption());
  EXPECT_TRUE(cache.Get(9, size, lkey.internal_key(), {}, noop).IsIOError());
  EXPECT_TRUE(cache.NewIterator(9, size)->status().IsIOError());
  EXPECT_TRUE(cache.Get(1, size, lkey.internal_key(), {}, noop).ok());  // Failures not cached.
}

// Many threads read across tables through small caches, forcing constant
// eviction of blocks and tables. Every value must be correct; TSan checks
// the synchronization.
TEST_F(TableCacheTest, ConcurrentReadersWithEviction) {
  constexpr uint64_t kTables = 6;
  for (uint64_t f = 1; f <= kTables; ++f) {
    BuildTable(f);
  }
  BlockCache block_cache(64 * 1024, 4);
  TableCache cache(dir_.path(), {.block_cache = &block_cache, .max_open_files = 2});
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t));
      for (int i = 0; i < 1500; ++i) {
        const uint64_t f = 1 + rng() % kTables;
        const int k = static_cast<int>(rng() % kKeysPerTable);
        ASSERT_EQ(Lookup(cache, f, k), ValueFor(f, k));
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

}  // namespace
}  // namespace talus
