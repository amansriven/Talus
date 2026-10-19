// Corruption tests (spec §11): flipping or truncating bytes in WAL, SST,
// MANIFEST and CURRENT files must give a Corruption error (or, for a WAL
// tail, a clean truncation), never a crash or wrong data.
//
// Each test builds a pristine database once, then for many positions copies
// it, damages one file, reopens it, and checks every key:
//  * WAL: the DB opens (non-paranoid), and the batches that survive are a
//    prefix of those written, each complete and correct. Replay stops at the
//    damage.
//  * SST: every key reads back either correctly or as Corruption. Never a
//    different value, and never NotFound for a key that exists.
//  * MANIFEST and CURRENT: Open fails with Corruption, or succeeds with every
//    key correct.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "Talus/db.h"
#include "test_util.h"
#include "util/file.h"
#include "util/filename.h"

namespace talus {
namespace {

namespace fs = std::filesystem;

constexpr int kKeysPerBatch = 3;

std::string BatchKey(int batch, int j) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "batch%05d_%d", batch, j);
  return buf;
}

std::string BatchValue(int batch, int j) {
  return "value-" + std::to_string(batch) + "-" + std::to_string(j) + std::string(60, 'v');
}

Options TestOptions() {
  Options o;
  o.create_if_missing = true;
  o.memtable_size = 64 << 10;
  o.block_size = 1024;
  o.target_file_size = 64 << 10;
  o.l1_max_bytes = 256 << 10;
  o.l0_compaction_trigger = 2;
  return o;
}

class CorruptionTest : public ::testing::Test {
 protected:
  // Writes batches [0, num_batches), one WriteBatch each.
  void WriteBatches(DB* db, int num_batches) {
    for (int b = 0; b < num_batches; ++b) {
      WriteBatch batch;
      for (int j = 0; j < kKeysPerBatch; ++j) {
        batch.Put(BatchKey(b, j), BatchValue(b, j));
      }
      ASSERT_TRUE(db->Write({}, &batch).ok());
    }
  }

  // The single file of `type` (the newest, if several).
  std::string FileOfType(const std::string& dir, FileType wanted) {
    std::string result;
    uint64_t best = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
      uint64_t number = 0;
      FileType type{};
      if (ParseFileName(entry.path().filename().string(), &number, &type) && type == wanted &&
          (result.empty() || number >= best)) {
        result = entry.path().string();
        best = number;
      }
    }
    return result;
  }

  std::vector<std::string> FilesOfType(const std::string& dir, FileType wanted) {
    std::vector<std::string> result;
    for (const auto& entry : fs::directory_iterator(dir)) {
      uint64_t number = 0;
      FileType type{};
      if (ParseFileName(entry.path().filename().string(), &number, &type) && type == wanted) {
        result.push_back(entry.path().string());
      }
    }
    return result;
  }

  // A fresh copy of the pristine DB in work_.
  std::string FreshCopy() {
    const std::string work = scratch_.File("work");
    fs::remove_all(work);
    fs::copy(golden_.path(), work);
    return work;
  }

  static void FlipByte(const std::string& path, uint64_t offset) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x5a);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&c, 1);
  }

  // Up to `n` offsets spread over [0, size), plus the last 64 bytes, where
  // footers and final records live.
  static std::vector<uint64_t> Offsets(uint64_t size, uint64_t n) {
    std::vector<uint64_t> offsets;
    for (uint64_t i = 0; i < n; ++i) {
      offsets.push_back(size * i / n);
    }
    for (uint64_t back = 1; back <= 64 && back <= size; ++back) {
      offsets.push_back(size - back);
    }
    return offsets;
  }

  // Checks the "prefix of batches, each complete and correct" property.
  // Returns the number of batches present.
  int CheckBatchPrefix(DB* db, int num_batches, const std::string& context) {
    int present_batches = 0;
    bool missing_seen = false;
    for (int b = 0; b < num_batches; ++b) {
      int present = 0;
      for (int j = 0; j < kKeysPerBatch; ++j) {
        std::string value;
        const Status s = db->Get({}, BatchKey(b, j), &value);
        if (s.ok()) {
          EXPECT_EQ(value, BatchValue(b, j)) << context;
          ++present;
        } else {
          EXPECT_TRUE(s.IsNotFound()) << context << ": " << s.ToString();
        }
      }
      EXPECT_TRUE(present == 0 || present == kKeysPerBatch)
          << context << ": batch " << b << " partially applied";
      if (present == 0) {
        missing_seen = true;
      } else {
        EXPECT_FALSE(missing_seen) << context << ": batch " << b << " present after a gap";
        ++present_batches;
      }
    }
    return present_batches;
  }

  test::ScopedTempDir golden_;
  test::ScopedTempDir scratch_;
};

// ---- WAL ----------------------------------------------------------------

TEST_F(CorruptionTest, WalByteFlipsTruncateReplayCleanly) {
  constexpr int kBatches = 150;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(TestOptions(), golden_.path(), &db).ok());
    WriteBatches(db.get(), kBatches);  // Stays in the memtable and WAL.
  }
  const uint64_t size = fs::file_size(FileOfType(golden_.path(), FileType::kLogFile));
  for (uint64_t offset : Offsets(size, 120)) {
    const std::string dir = FreshCopy();
    FlipByte(FileOfType(dir, FileType::kLogFile), offset);
    const std::string context = "WAL flip at " + std::to_string(offset);

    Options paranoid = TestOptions();
    paranoid.paranoid_checks = true;
    {
      // Paranoid: a damaged record is Corruption, unless the damage makes it
      // look like a torn tail (a length past EOF), which is a clean end.
      std::unique_ptr<DB> db;
      const Status s = DB::Open(paranoid, dir, &db);
      if (s.ok()) {
        CheckBatchPrefix(db.get(), kBatches, context + " (paranoid)");
      } else {
        EXPECT_TRUE(s.IsCorruption()) << context << ": " << s.ToString();
      }
    }
    // Paranoid open may have failed before changing anything. Start fresh.
    const std::string dir2 = FreshCopy();
    FlipByte(FileOfType(dir2, FileType::kLogFile), offset);
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(TestOptions(), dir2, &db).ok()) << context;
    const int present = CheckBatchPrefix(db.get(), kBatches, context);
    EXPECT_LT(present, kBatches) << context << ": damage went unnoticed";
  }
}

TEST_F(CorruptionTest, WalTruncationIsACleanTail) {
  constexpr int kBatches = 150;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(TestOptions(), golden_.path(), &db).ok());
    WriteBatches(db.get(), kBatches);
  }
  const uint64_t size = fs::file_size(FileOfType(golden_.path(), FileType::kLogFile));
  std::vector<uint64_t> lengths;
  for (uint64_t len = 0; len < size; len += std::max<uint64_t>(1, size / 150)) {
    lengths.push_back(len);
  }
  lengths.push_back(size);  // Untruncated.
  int previous = -1;
  for (uint64_t len : lengths) {
    const std::string dir = FreshCopy();
    fs::resize_file(FileOfType(dir, FileType::kLogFile), len);
    Options paranoid = TestOptions();
    paranoid.paranoid_checks = true;  // A torn tail is not corruption.
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(paranoid, dir, &db).ok()) << "truncated to " << len;
    const int present = CheckBatchPrefix(db.get(), kBatches, "WAL truncated to " + std::to_string(len));
    EXPECT_GE(present, previous) << "a longer WAL recovered fewer batches";
    previous = present;
  }
  EXPECT_EQ(previous, kBatches);  // The final, untruncated length recovers all.
}

// ---- SSTables -----------------------------------------------------------------

TEST_F(CorruptionTest, SstDamageIsCorruptionNeverWrongData) {
  constexpr int kBatches = 600;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(TestOptions(), golden_.path(), &db).ok());
    WriteBatches(db.get(), kBatches);
    ASSERT_TRUE(db->CompactRange("", "").ok());  // Everything in SSTables.
  }
  const std::vector<std::string> tables = FilesOfType(golden_.path(), FileType::kTableFile);
  ASSERT_FALSE(tables.empty());
  int corruption_reports = 0;
  for (const std::string& golden_table : tables) {
    const std::string name = fs::path(golden_table).filename().string();
    const uint64_t size = fs::file_size(golden_table);
    std::vector<std::pair<uint64_t, bool>> damage;  // (offset or length, is_truncation)
    for (uint64_t offset : Offsets(size, 40)) {
      damage.emplace_back(offset, false);
    }
    damage.emplace_back(size / 2, true);
    damage.emplace_back(size - 1, true);

    for (const auto& [where, truncate] : damage) {
      const std::string dir = FreshCopy();
      const std::string table = dir + "/" + name;
      if (truncate) {
        fs::resize_file(table, where);
      } else {
        FlipByte(table, where);
      }
      const std::string context = name + (truncate ? " truncated to " : " flipped at ") +
                                  std::to_string(where);
      std::unique_ptr<DB> db;
      ASSERT_TRUE(DB::Open(TestOptions(), dir, &db).ok()) << context;  // Tables open lazily.
      for (int b = 0; b < kBatches; ++b) {
        std::string value;
        const Status s = db->Get({}, BatchKey(b, 0), &value);
        if (s.ok()) {
          ASSERT_EQ(value, BatchValue(b, 0)) << context << ": wrong data";
        } else {
          ASSERT_TRUE(s.IsCorruption()) << context << ": " << s.ToString();
          ++corruption_reports;
        }
      }
      // A scan returns a correct prefix, ending at the end or at Corruption.
      auto iter = db->NewIterator({});
      int seen = 0;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++seen) {
        const int b = seen / kKeysPerBatch;
        ASSERT_EQ(iter->key(), BatchKey(b, seen % kKeysPerBatch)) << context;
        ASSERT_EQ(iter->value(), BatchValue(b, seen % kKeysPerBatch)) << context;
      }
      ASSERT_TRUE(iter->status().ok() || iter->status().IsCorruption()) << context;
      if (iter->status().ok()) {
        EXPECT_EQ(seen, kBatches * kKeysPerBatch) << context;
      }
    }
  }
  EXPECT_GT(corruption_reports, 0);  // The damage was actually detected.
}

// ---- MANIFEST and CURRENT -------------------------------------------------------

class MetadataCorruptionTest : public CorruptionTest {
 protected:
  static constexpr int kBatches = 900;

  void SetUp() override {
    // Several flushes and compactions give the MANIFEST many records, and
    // the last batches stay in the WAL.
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(TestOptions(), golden_.path(), &db).ok());
    WriteBatches(db.get(), kBatches);
    ASSERT_TRUE(db->WaitForBackgroundWork().ok());
  }

  // Opens a damaged copy: Corruption, or success with every key correct.
  void ExpectCorruptionOrCorrect(const std::string& dir, const std::string& context) {
    std::unique_ptr<DB> db;
    const Status s = DB::Open(TestOptions(), dir, &db);
    if (!s.ok()) {
      EXPECT_TRUE(s.IsCorruption()) << context << ": " << s.ToString();
      return;
    }
    for (int b = 0; b < kBatches; ++b) {
      for (int j = 0; j < kKeysPerBatch; ++j) {
        std::string value;
        const Status gs = db->Get({}, BatchKey(b, j), &value);
        ASSERT_TRUE(gs.ok()) << context << ": opened, but " << BatchKey(b, j) << " is "
                             << gs.ToString();
        ASSERT_EQ(value, BatchValue(b, j)) << context;
      }
    }
  }
};

TEST_F(MetadataCorruptionTest, ManifestDamageIsCorruptionNeverWrongData) {
  const std::string name = fs::path(FileOfType(golden_.path(), FileType::kDescriptorFile)).filename();
  const uint64_t size = fs::file_size(golden_.path() + "/" + name);
  for (uint64_t offset : Offsets(size, 80)) {
    const std::string dir = FreshCopy();
    FlipByte(dir + "/" + name, offset);
    ExpectCorruptionOrCorrect(dir, "MANIFEST flipped at " + std::to_string(offset));
  }
  for (uint64_t len = 0; len < size; len += std::max<uint64_t>(1, size / 60)) {
    const std::string dir = FreshCopy();
    fs::resize_file(dir + "/" + name, len);
    ExpectCorruptionOrCorrect(dir, "MANIFEST truncated to " + std::to_string(len));
  }
}

TEST_F(MetadataCorruptionTest, CurrentDamageIsCorruption) {
  const uint64_t size = fs::file_size(CurrentFileName(golden_.path()));
  for (uint64_t offset = 0; offset < size; ++offset) {
    const std::string dir = FreshCopy();
    FlipByte(CurrentFileName(dir), offset);
    ExpectCorruptionOrCorrect(dir, "CURRENT flipped at " + std::to_string(offset));
  }
  for (uint64_t len : {uint64_t{0}, size / 2, size - 1}) {
    const std::string dir = FreshCopy();
    fs::resize_file(CurrentFileName(dir), len);
    ExpectCorruptionOrCorrect(dir, "CURRENT truncated to " + std::to_string(len));
  }
}

}  // namespace
}  // namespace talus
