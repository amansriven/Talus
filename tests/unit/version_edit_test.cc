#include "version/version_edit.h"

#include <gtest/gtest.h>

#include <string>

#include "util/coding.h"

namespace talus {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, ValueType::kValue);
  return k;
}

VersionEdit SampleEdit() {
  VersionEdit edit;
  edit.SetLogNumber(17);
  edit.SetNextFileNumber(1000);
  edit.SetLastSequence(kMaxSequenceNumber);
  edit.SetCompactPointer(2, IKey("pointer", 5));
  edit.DeleteFile(1, 42);
  edit.DeleteFile(3, 7);
  for (int i = 0; i < 4; ++i) {
    FileMetaData f;
    f.number = 100 + static_cast<uint64_t>(i);
    f.file_size = 1ULL << (20 + i);
    f.smallest = IKey("a" + std::to_string(i), 9);
    f.largest = IKey("z" + std::to_string(i), 1);
    edit.AddFile(i, f);
  }
  return edit;
}

TEST(VersionEditTest, RoundTrip) {
  const VersionEdit edit = SampleEdit();
  std::string encoded;
  edit.EncodeTo(&encoded);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  std::string reencoded;
  decoded.EncodeTo(&reencoded);
  EXPECT_EQ(encoded, reencoded);
  EXPECT_EQ(decoded.log_number, 17u);
  EXPECT_EQ(decoded.next_file_number, 1000u);
  EXPECT_EQ(decoded.last_sequence, kMaxSequenceNumber);
  ASSERT_EQ(decoded.new_files.size(), 4u);
  EXPECT_EQ(decoded.new_files[3].first, 3);
  EXPECT_EQ(decoded.new_files[3].second.file_size, 1ULL << 23);
  EXPECT_EQ(decoded.new_files[3].second.largest, IKey("z3", 1));
  EXPECT_EQ(decoded.deleted_files.size(), 2u);
  EXPECT_EQ(decoded.compact_pointers[0].second, IKey("pointer", 5));
}

TEST(VersionEditTest, EmptyEditRoundTrips) {
  std::string encoded;
  VersionEdit().EncodeTo(&encoded);
  EXPECT_TRUE(encoded.empty());
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_FALSE(decoded.log_number.has_value());
}

TEST(VersionEditTest, EveryTruncationIsCorruptionOrAShorterValidEdit) {
  std::string encoded;
  SampleEdit().EncodeTo(&encoded);
  for (size_t len = 0; len < encoded.size(); ++len) {
    VersionEdit decoded;
    const Status s = decoded.DecodeFrom(std::string_view(encoded).substr(0, len));
    // Field boundaries decode as a prefix edit; every other cut must fail.
    if (s.ok()) {
      std::string prefix;
      decoded.EncodeTo(&prefix);
      EXPECT_EQ(prefix, encoded.substr(0, len)) << len;
    } else {
      EXPECT_TRUE(s.IsCorruption()) << len;
    }
  }
}

TEST(VersionEditTest, RejectsInvalidFields) {
  VersionEdit decoded;
  std::string unknown_tag;
  PutVarint32(&unknown_tag, 99);
  EXPECT_TRUE(decoded.DecodeFrom(unknown_tag).IsCorruption());

  std::string bad_level;
  PutVarint32(&bad_level, 5);  // deleted_file
  PutVarint32(&bad_level, VersionEdit::kMaxLevels);
  PutVarint64(&bad_level, 1);
  EXPECT_TRUE(decoded.DecodeFrom(bad_level).IsCorruption());

  std::string short_key;
  PutVarint32(&short_key, 4);  // compact_pointer
  PutVarint32(&short_key, 1);
  PutLengthPrefixed(&short_key, "short");  // Under 8 bytes: not an internal key.
  EXPECT_TRUE(decoded.DecodeFrom(short_key).IsCorruption());

  std::string bad_sequence;
  PutVarint32(&bad_sequence, 3);  // last_sequence beyond 56 bits
  PutVarint64(&bad_sequence, kMaxSequenceNumber + 1);
  EXPECT_TRUE(decoded.DecodeFrom(bad_sequence).IsCorruption());
}

}  // namespace
}  // namespace talus
