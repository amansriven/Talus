#include "db/log_replay.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "test_util.h"
#include "wal/log_format.h"
#include "wal/log_writer.h"

namespace talus {
namespace {

std::string Get(const MemTable& mem, std::string_view key) {
  LookupKey lkey(key, kMaxSequenceNumber);
  std::string_view value;
  Status s;
  if (!mem.Get(lkey, &value, &s)) {
    return "<absent>";
  }
  return s.IsNotFound() ? "<deleted>" : std::string(value);
}

class LogReplayTest : public ::testing::Test {
 protected:
  // Writes three batches: seq 10 {a=1, b=2}, seq 12 {delete a}, seq 13 {c=3}.
  void WriteThreeBatches() {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Create(path_, &file).ok());
    log::Writer writer(std::move(file));
    WriteBatch b1;
    b1.Put("a", "1");
    b1.Put("b", "2");
    WriteBatchInternal::SetSequence(&b1, 10);
    WriteBatch b2;
    b2.Delete("a");
    WriteBatchInternal::SetSequence(&b2, 12);
    WriteBatch b3;
    b3.Put("c", "3");
    WriteBatchInternal::SetSequence(&b3, 13);
    for (const WriteBatch* b : {&b1, &b2, &b3}) {
      ASSERT_TRUE(writer.AddRecord(WriteBatchInternal::Contents(*b)).ok());
      record_ends_.push_back(writer.file()->size());
    }
    ASSERT_TRUE(writer.file()->Close().ok());
  }

  void CorruptByteAt(uint64_t offset) {
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(static_cast<std::streamoff>(offset));
    char c;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x55);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&c, 1);
  }

  test::ScopedTempDir dir_;
  const std::string path_ = dir_.File("000007.log");
  std::vector<uint64_t> record_ends_;
};

TEST_F(LogReplayTest, ReplaysAllBatchesInOrder) {
  WriteThreeBatches();
  MemTable mem;
  SequenceNumber max_seq = 0;
  ASSERT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/true, &mem, &max_seq).ok());
  EXPECT_EQ(max_seq, 13u);
  EXPECT_EQ(Get(mem, "a"), "<deleted>");
  EXPECT_EQ(Get(mem, "b"), "2");
  EXPECT_EQ(Get(mem, "c"), "3");
}

TEST_F(LogReplayTest, CorruptRecordDropsItAndEverythingAfter) {
  WriteThreeBatches();
  CorruptByteAt(record_ends_[0] + log::kHeaderSize + 3);  // Inside batch 2's payload.

  MemTable mem;
  SequenceNumber max_seq = 0;
  ASSERT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/false, &mem, &max_seq).ok());
  EXPECT_EQ(max_seq, 11u);
  EXPECT_EQ(Get(mem, "a"), "1");         // The delete in batch 2 was dropped...
  EXPECT_EQ(Get(mem, "c"), "<absent>");  // ...and so was the intact batch 3.

  MemTable strict;
  max_seq = 0;
  EXPECT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/true, &strict, &max_seq).IsCorruption());
}

TEST_F(LogReplayTest, TornTailIsNotCorruptionEvenWhenParanoid) {
  WriteThreeBatches();
  std::filesystem::resize_file(path_, record_ends_[2] - 2);

  MemTable mem;
  SequenceNumber max_seq = 0;
  ASSERT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/true, &mem, &max_seq).ok());
  EXPECT_EQ(max_seq, 12u);
  EXPECT_EQ(Get(mem, "a"), "<deleted>");
  EXPECT_EQ(Get(mem, "c"), "<absent>");
}

TEST_F(LogReplayTest, MalformedBatchIsNeverHalfApplied) {
  // A record with a valid CRC whose batch claims 3 ops but holds 1.
  WriteBatch b;
  b.Put("x", "1");
  WriteBatchInternal::SetSequence(&b, 5);
  WriteBatchInternal::SetCount(&b, 3);
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Create(path_, &file).ok());
    log::Writer writer(std::move(file));
    ASSERT_TRUE(writer.AddRecord(WriteBatchInternal::Contents(b)).ok());
  }
  MemTable mem;
  SequenceNumber max_seq = 0;
  ASSERT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/false, &mem, &max_seq).ok());
  EXPECT_EQ(Get(mem, "x"), "<absent>");
  EXPECT_EQ(max_seq, 0u);

  MemTable strict;
  EXPECT_TRUE(ReplayLogFile(path_, /*paranoid_checks=*/true, &strict, &max_seq).IsCorruption());
}

TEST_F(LogReplayTest, MissingFileIsIOError) {
  MemTable mem;
  SequenceNumber max_seq = 0;
  EXPECT_TRUE(ReplayLogFile(dir_.File("missing.log"), false, &mem, &max_seq).IsIOError());
}

}  // namespace
}  // namespace talus
