#include "table/table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "table/table_builder.h"
#include "test_util.h"
#include "util/file.h"

namespace talus {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;  // Sorted by internal key.

std::string IKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, type);
  return k;
}

// 3000 user keys with 1-3 versions each, some deletions.
Entries SampleEntries() {
  std::mt19937 rng(301);
  Entries entries;
  SequenceNumber seq = 100000;
  for (int i = 0; i < 3000; ++i) {
    char user_key[16];
    std::snprintf(user_key, sizeof(user_key), "user%05d", i * 2);  // Gaps for absent keys.
    const int versions = 1 + static_cast<int>(rng() % 3);
    for (int v = 0; v < versions; ++v) {
      const bool deletion = rng() % 7 == 0;
      entries.emplace_back(
          IKey(user_key, seq--, deletion ? ValueType::kDeletion : ValueType::kValue),
          deletion ? "" : std::string(rng() % 200, static_cast<char>('a' + v)));
    }
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return InternalKeyComparator()(a.first, b.first) < 0;
  });
  return entries;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::string& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

Status BuildTable(const std::string& path, const Entries& entries, const TableOptions& options,
                  uint64_t* file_size = nullptr) {
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Create(path, &file);
  if (!s.ok()) {
    return s;
  }
  TableBuilder builder(options, file.get());
  for (const auto& [k, v] : entries) {
    builder.Add(k, v);
  }
  s = builder.Finish();
  if (s.ok()) {
    s = file->Close();
  }
  if (file_size != nullptr) {
    *file_size = builder.FileSize();
  }
  return s;
}

Status OpenTable(const std::string& path, RandomAccessFile::Mode mode, std::unique_ptr<Table>* t) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(path, mode, &file);
  return s.ok() ? Table::Open(TableOpenOptions{}, std::move(file), t) : s;
}

// Parameters: (read mode, filter type, block size).
class TableTest
    : public ::testing::TestWithParam<std::tuple<RandomAccessFile::Mode, FilterType, size_t>> {
 protected:
  RandomAccessFile::Mode mode() const { return std::get<0>(GetParam()); }
  TableOptions options() const {
    TableOptions o;
    o.filter_type = std::get<1>(GetParam());
    o.block_size = std::get<2>(GetParam());
    return o;
  }

  test::ScopedTempDir dir_;
  const std::string path_ = dir_.File("000005.sst");
  const Entries entries_ = SampleEntries();
};

std::string ConfigName(const ::testing::TestParamInfo<TableTest::ParamType>& info) {
  const auto [mode, filter, block_size] = info.param;
  const char* const filters[] = {"NoFilter", "Standard", "Blocked"};
  return std::string(mode == RandomAccessFile::Mode::kMmap ? "Mmap" : "Pread") + "_" +
         filters[static_cast<int>(filter)] + "_Block" + std::to_string(block_size);
}

INSTANTIATE_TEST_SUITE_P(
    Configs, TableTest,
    ::testing::Combine(
        ::testing::Values(RandomAccessFile::Mode::kPread, RandomAccessFile::Mode::kMmap),
        ::testing::Values(FilterType::kNone, FilterType::kStandardBloom, FilterType::kBlockedBloom),
        ::testing::Values(size_t{256}, size_t{4096}, size_t{65536})),
    ConfigName);

TEST_P(TableTest, IterateAndSeekMatchEntries) {
  ASSERT_TRUE(BuildTable(path_, entries_, options()).ok());
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path_, mode(), &table).ok());
  auto iter = table->NewIterator();

  iter->SeekToFirst();
  for (const auto& [k, v] : entries_) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key(), k);
    ASSERT_EQ(iter->value(), v);
    iter->Next();
  }
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());

  // Seek to lookup keys of present and absent user keys at assorted
  // sequences; the result must be the entries' lower bound.
  std::mt19937 rng(9);
  for (int i = 0; i < 2000; ++i) {
    char user_key[16];
    std::snprintf(user_key, sizeof(user_key), "user%05d", static_cast<int>(rng() % 6100));
    const LookupKey lkey(user_key, rng() % 110000);
    auto lb = std::lower_bound(
        entries_.begin(), entries_.end(), lkey.internal_key(),
        [](const auto& e, std::string_view t) { return InternalKeyComparator()(e.first, t) < 0; });
    iter->Seek(lkey.internal_key());
    if (lb == entries_.end()) {
      EXPECT_FALSE(iter->Valid());
    } else {
      ASSERT_TRUE(iter->Valid());
      EXPECT_EQ(iter->key(), lb->first);
    }
  }
}

TEST_P(TableTest, InternalGetFindsNewestVisibleVersion) {
  ASSERT_TRUE(BuildTable(path_, entries_, options()).ok());
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path_, mode(), &table).ok());

  for (const auto& [k, v] : entries_) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(k, &parsed));
    // A lookup at exactly this entry's sequence must land on this entry.
    const LookupKey lkey(parsed.user_key, parsed.sequence);
    bool called = false;
    ASSERT_TRUE(table
                    ->InternalGet(lkey.internal_key(), TableReadOptions{},
                                  [&](std::string_view found_key, std::string_view found_value,
                                      Pin& /*pin*/) {
                                    called = true;
                                    EXPECT_EQ(found_key, k);
                                    EXPECT_EQ(found_value, v);
                                  })
                    .ok());
    ASSERT_TRUE(called);
  }

  // Absent user keys: either the filter skips the table, or the callback
  // sees a different user key.
  int filtered = 0;
  for (int i = 0; i < 3000; ++i) {
    char user_key[16];
    std::snprintf(user_key, sizeof(user_key), "user%05d", i * 2 + 1);
    const LookupKey lkey(user_key, kMaxSequenceNumber);
    bool called = false;
    ASSERT_TRUE(table
                    ->InternalGet(lkey.internal_key(), TableReadOptions{},
                                  [&](std::string_view found_key, std::string_view, Pin& /*pin*/) {
                                    called = true;
                                    EXPECT_NE(ExtractUserKey(found_key), user_key);
                                  })
                    .ok());
    filtered += table->KeyMayMatch(user_key) ? 0 : 1;
    if (!table->KeyMayMatch(user_key)) {
      EXPECT_FALSE(called);
    }
  }
  if (options().filter_type == FilterType::kNone) {
    EXPECT_EQ(filtered, 0);
  } else {
    EXPECT_GT(filtered, 3000 * 95 / 100);  // FPR is about 1%.
  }
}

TEST_P(TableTest, EveryByteFlipIsDetectedOrHarmless) {
  ASSERT_TRUE(BuildTable(path_, entries_, options()).ok());
  const std::string good = ReadFile(path_);
  // Sample positions across the file; the footer is covered densely.
  std::vector<size_t> positions;
  for (size_t p = 0; p < good.size(); p += std::max<size_t>(1, good.size() / 300)) {
    positions.push_back(p);
  }
  for (size_t p = good.size() - Footer::kEncodedLength; p < good.size(); ++p) {
    positions.push_back(p);
  }
  for (size_t pos : positions) {
    std::string bad = good;
    bad[pos] = static_cast<char>(bad[pos] ^ 0x41);
    WriteFile(path_, bad);
    std::unique_ptr<Table> table;
    Status s = OpenTable(path_, mode(), &table);
    if (!s.ok()) {
      EXPECT_TRUE(s.IsCorruption()) << pos << ": " << s.ToString();
      continue;
    }
    // Whatever is read back must be a correct prefix of the entries, ending
    // either at the end or at a Corruption status. Never wrong data.
    auto iter = table->NewIterator();
    size_t i = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++i) {
      ASSERT_LT(i, entries_.size()) << pos;
      ASSERT_EQ(iter->key(), entries_[i].first) << pos;
      ASSERT_EQ(iter->value(), entries_[i].second) << pos;
    }
    if (iter->status().ok()) {
      EXPECT_EQ(i, entries_.size()) << pos;
    } else {
      EXPECT_TRUE(iter->status().IsCorruption()) << pos;
    }
  }
}

TEST_P(TableTest, TruncatedFileFailsToOpen) {
  ASSERT_TRUE(BuildTable(path_, entries_, options()).ok());
  const std::string good = ReadFile(path_);
  for (size_t len :
       {size_t{0}, size_t{10}, Footer::kEncodedLength - 1, good.size() / 2, good.size() - 1}) {
    WriteFile(path_, std::string_view(good).substr(0, len));
    std::unique_ptr<Table> table;
    EXPECT_TRUE(OpenTable(path_, mode(), &table).IsCorruption()) << len;
  }
}

class TableFormatTest : public ::testing::Test {
 protected:
  test::ScopedTempDir dir_;
  const std::string path_ = dir_.File("000009.sst");
};

TEST_F(TableFormatTest, FooterIsLast48BytesWithMagic) {
  uint64_t size = 0;
  ASSERT_TRUE(BuildTable(path_, SampleEntries(), TableOptions{}, &size).ok());
  const std::string bytes = ReadFile(path_);
  ASSERT_EQ(bytes.size(), size);
  EXPECT_EQ(bytes.substr(bytes.size() - 8), "TalusSST");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(std::string_view(bytes).substr(bytes.size() - 48)).ok());
  EXPECT_GT(footer.filter.size, 0u);
  // Layout: data blocks, then filter, then index, then footer.
  EXPECT_EQ(footer.index.offset, footer.filter.offset + footer.filter.size + kBlockTrailerSize);
  EXPECT_EQ(footer.index.offset + footer.index.size + kBlockTrailerSize, bytes.size() - 48);
}

TEST_F(TableFormatTest, EmptyTable) {
  ASSERT_TRUE(BuildTable(path_, {}, TableOptions{}).ok());
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path_, RandomAccessFile::Mode::kPread, &table).ok());
  auto iter = table->NewIterator();
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(TableFormatTest, Lz4CompressesWhenAvailable) {
  Entries entries;
  for (int i = 0; i < 2000; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%05d", i);
    entries.emplace_back(IKey(key, 1, ValueType::kValue), std::string(300, 'x'));  // Compressible.
  }
  TableOptions lz4;
  lz4.compression = CompressionType::kLZ4;
  if (!CompressionSupported(CompressionType::kLZ4)) {
    EXPECT_TRUE(BuildTable(path_, entries, lz4).IsInvalidArgument());
    return;
  }
  uint64_t plain_size = 0;
  uint64_t lz4_size = 0;
  ASSERT_TRUE(BuildTable(dir_.File("plain.sst"), entries, TableOptions{}, &plain_size).ok());
  ASSERT_TRUE(BuildTable(path_, entries, lz4, &lz4_size).ok());
  EXPECT_LT(lz4_size, plain_size / 4);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(path_, RandomAccessFile::Mode::kPread, &table).ok());
  auto iter = table->NewIterator();
  size_t i = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++i) {
    ASSERT_EQ(iter->key(), entries[i].first);
    ASSERT_EQ(iter->value(), entries[i].second);
  }
  EXPECT_EQ(i, entries.size());
  EXPECT_TRUE(iter->status().ok());
}

}  // namespace
}  // namespace talus
