#include "db/dbformat.h"

#include <gtest/gtest.h>

#include <string>

namespace talus {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string result;
  AppendInternalKey(&result, user_key, seq, type);
  return result;
}

TEST(DbFormatTest, InternalKeyRoundTrip) {
  const std::string keys[] = {"", "k", "hello", "longggggggggggggggggggggg"};
  const SequenceNumber seqs[] = {1,
                                 2,
                                 3,
                                 (SequenceNumber{1} << 8) - 1,
                                 SequenceNumber{1} << 8,
                                 (SequenceNumber{1} << 32) + 1,
                                 kMaxSequenceNumber};
  for (const std::string& key : keys) {
    for (SequenceNumber seq : seqs) {
      for (ValueType type : {ValueType::kValue, ValueType::kDeletion}) {
        const std::string ikey = IKey(key, seq, type);
        ASSERT_EQ(ikey.size(), key.size() + kInternalKeyTagSize);
        ParsedInternalKey parsed;
        ASSERT_TRUE(ParseInternalKey(ikey, &parsed));
        EXPECT_EQ(parsed.user_key, key);
        EXPECT_EQ(parsed.sequence, seq);
        EXPECT_EQ(parsed.type, type);
        EXPECT_EQ(ExtractUserKey(ikey), key);
      }
    }
  }
}

TEST(DbFormatTest, ParseRejectsMalformedKeys) {
  ParsedInternalKey parsed;
  EXPECT_FALSE(ParseInternalKey("", &parsed));
  EXPECT_FALSE(ParseInternalKey("1234567", &parsed));
  std::string bad_type = IKey("foo", 5, ValueType::kValue);
  bad_type[3] = 2;  // The type byte is the tag's least significant byte.
  EXPECT_FALSE(ParseInternalKey(bad_type, &parsed));
}

TEST(DbFormatTest, ComparatorOrdersUserKeyAscendingThenSequenceDescending) {
  const InternalKeyComparator cmp;
  // Different user keys: bytewise, regardless of sequence.
  EXPECT_LT(cmp(IKey("a", 1, ValueType::kValue), IKey("b", 100, ValueType::kValue)), 0);
  EXPECT_LT(cmp(IKey("a", 1, ValueType::kValue), IKey("ab", 1, ValueType::kValue)), 0);
  EXPECT_LT(cmp(IKey("", 1, ValueType::kValue), IKey("a", 1, ValueType::kValue)), 0);
  EXPECT_LT(cmp(IKey("\x7f", 1, ValueType::kValue), IKey("\x80", 1, ValueType::kValue)), 0);
  // Same user key: newer sequence first.
  EXPECT_LT(cmp(IKey("foo", 100, ValueType::kValue), IKey("foo", 99, ValueType::kValue)), 0);
  EXPECT_GT(cmp(IKey("foo", 1, ValueType::kValue), IKey("foo", 2, ValueType::kDeletion)), 0);
  // Same sequence: higher type first.
  EXPECT_LT(cmp(IKey("foo", 5, ValueType::kValue), IKey("foo", 5, ValueType::kDeletion)), 0);
  EXPECT_EQ(cmp(IKey("foo", 5, ValueType::kValue), IKey("foo", 5, ValueType::kValue)), 0);
}

TEST(DbFormatTest, LookupKeyEncodesSeekTag) {
  for (size_t len : {size_t{0}, size_t{16}, size_t{192}, size_t{193}, size_t{1000}}) {
    const std::string user_key(len, 'x');
    LookupKey lkey(user_key, 42);
    EXPECT_EQ(lkey.user_key(), user_key);
    EXPECT_EQ(lkey.internal_key(), IKey(user_key, 42, kValueTypeForSeek));
  }
}

}  // namespace
}  // namespace talus
