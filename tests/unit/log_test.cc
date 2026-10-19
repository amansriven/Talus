#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/log_format.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace talus::log {
namespace {

std::string MakeRecord(size_t len, int seed) {
  std::string s(len, '\0');
  for (size_t i = 0; i < len; ++i) {
    s[i] = static_cast<char>((i * 7 + static_cast<size_t>(seed) * 13) & 0xff);
  }
  return s;
}

void WriteLog(const std::string& path, const std::vector<std::string>& records) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(WritableFile::Create(path, &file).ok());
  Writer writer(std::move(file));
  for (const std::string& r : records) {
    ASSERT_TRUE(writer.AddRecord(r).ok());
  }
  ASSERT_TRUE(writer.Sync().ok());
  ASSERT_TRUE(writer.file()->Close().ok());
}

std::unique_ptr<Reader> OpenReader(const std::string& path) {
  std::unique_ptr<SequentialFile> file;
  EXPECT_TRUE(SequentialFile::Open(path, &file).ok());
  return std::make_unique<Reader>(std::move(file));
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteAll(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

class LogTest : public ::testing::Test {
 protected:
  test::ScopedTempDir dir_;
  const std::string path_ = dir_.File("000001.log");
};

TEST_F(LogTest, EmptyLog) {
  WriteLog(path_, {});
  auto reader = OpenReader(path_);
  std::string scratch;
  std::string_view record;
  EXPECT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kEof);
}

TEST_F(LogTest, RoundTripVariousSizes) {
  // Sizes around the 64 KiB file buffers exercise their partial-fill,
  // exact-fill and bypass paths.
  const size_t sizes[] = {0, 1, 100, 65535 - kHeaderSize, 65536, 65537, 200000, 3, 0, 10};
  std::vector<std::string> records;
  for (size_t i = 0; i < std::size(sizes); ++i) {
    records.push_back(MakeRecord(sizes[i], static_cast<int>(i)));
  }
  WriteLog(path_, records);

  size_t expected_size = 0;
  for (const auto& r : records) {
    expected_size += kHeaderSize + r.size();
  }
  EXPECT_EQ(std::filesystem::file_size(path_), expected_size);

  auto reader = OpenReader(path_);
  std::string scratch;
  std::string_view record;
  for (const std::string& expected : records) {
    ASSERT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kRecord);
    EXPECT_EQ(record, expected);
  }
  EXPECT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kEof);
}

TEST_F(LogTest, ExactHeaderFormat) {
  WriteLog(path_, {"abc"});
  const std::string bytes = ReadAll(path_);
  ASSERT_EQ(bytes.size(), kHeaderSize + 3);
  const std::string typed_payload = std::string(1, static_cast<char>(kFullType)) + "abc";
  EXPECT_EQ(crc32c::Unmask(DecodeFixed32(bytes.data())),
            crc32c::Value(typed_payload.data(), typed_payload.size()));
  EXPECT_EQ(DecodeFixed32(bytes.data() + 4), 3u);
  EXPECT_EQ(bytes[8], static_cast<char>(kFullType));
  EXPECT_EQ(bytes.substr(9), "abc");
}

// A crash can cut the last record anywhere. Every such cut must read back as
// the earlier records followed by a clean end of file.
TEST_F(LogTest, TornTailIsCleanEof) {
  const std::vector<std::string> records = {MakeRecord(50, 1), MakeRecord(70, 2),
                                            MakeRecord(90, 3)};
  WriteLog(path_, records);
  const std::string full = ReadAll(path_);
  const size_t third_start = 2 * kHeaderSize + 50 + 70;
  for (size_t len = third_start; len < full.size(); ++len) {
    WriteAll(path_, full.substr(0, len));
    auto reader = OpenReader(path_);
    std::string scratch;
    std::string_view record;
    ASSERT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kRecord) << len;
    ASSERT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kRecord) << len;
    EXPECT_EQ(record, records[1]);
    ASSERT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kEof) << len;
  }
}

TEST_F(LogTest, FlippedBitIsCorruption) {
  WriteLog(path_, {MakeRecord(50, 1), MakeRecord(50, 2)});
  const std::string full = ReadAll(path_);
  // Flip one bit in each byte of the second record (CRC, length excluded,
  // type, payload). Length flips are covered separately below.
  const size_t second = kHeaderSize + 50;
  for (size_t pos :
       {second + 0, second + 3, second + 8, second + 9, second + 30, full.size() - 1}) {
    std::string corrupt = full;
    corrupt[pos] = static_cast<char>(corrupt[pos] ^ 0x10);
    WriteAll(path_, corrupt);
    auto reader = OpenReader(path_);
    std::string scratch;
    std::string_view record;
    ASSERT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kRecord) << pos;
    EXPECT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kCorruption) << pos;
    EXPECT_TRUE(reader->error().IsCorruption());
  }
}

TEST_F(LogTest, CorruptLengthNeverMisreads) {
  WriteLog(path_, {MakeRecord(50, 1), MakeRecord(50, 2)});
  const std::string full = ReadAll(path_);
  // A shorter length fails the CRC; a length past EOF looks like a torn tail,
  // including 0xffffffff, which must not trigger a 4 GiB allocation.
  for (uint32_t bad_len : {49u, 51u, 1000u, 0xffffffffu}) {
    std::string corrupt = full;
    EncodeFixed32(corrupt.data() + 4, bad_len);
    WriteAll(path_, corrupt);
    auto reader = OpenReader(path_);
    std::string scratch;
    std::string_view record;
    const Reader::Result r = reader->ReadRecord(&record, &scratch);
    EXPECT_NE(r, Reader::Result::kRecord) << bad_len;
  }
}

TEST_F(LogTest, UnknownTypeWithValidCrcIsCorruption) {
  const std::string payload = "data";
  std::string typed = std::string(1, '\x02') + payload;
  std::string rec(kHeaderSize, '\0');
  EncodeFixed32(rec.data(), crc32c::Mask(crc32c::Value(typed.data(), typed.size())));
  EncodeFixed32(rec.data() + 4, static_cast<uint32_t>(payload.size()));
  rec[8] = '\x02';
  WriteAll(path_, rec + payload);
  auto reader = OpenReader(path_);
  std::string scratch;
  std::string_view record;
  EXPECT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kCorruption);
}

TEST_F(LogTest, ZeroFilledRegionIsCorruption) {
  WriteAll(path_, std::string(64, '\0'));
  auto reader = OpenReader(path_);
  std::string scratch;
  std::string_view record;
  EXPECT_EQ(reader->ReadRecord(&record, &scratch), Reader::Result::kCorruption);
}

}  // namespace
}  // namespace talus::log
