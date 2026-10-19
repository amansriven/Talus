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

TEST(DbFormatTest, ShortestSeparator) {
  const std::string max_tag_abd = IKey("abd", kMaxSequenceNumber, kValueTypeForSeek);
  std::string s = IKey("abcdefg", 5, ValueType::kValue);
  InternalKeyComparator::FindShortestSeparator(&s, IKey("abzzz", 1, ValueType::kValue));
  EXPECT_EQ(s, max_tag_abd);

  // Left unchanged when nothing shorter fits.
  const std::string cases[][2] = {
      {"foo", "foo"},     // Same user key (different sequence).
      {"foo", "foobar"},  // Prefix.
      {"abc1", "abc2"},   // Adjacent bytes.
      {"a\xff", "b"},     // 0xff cannot be incremented.
  };
  for (const auto& [start, limit] : cases) {
    const std::string original = IKey(start, 100, ValueType::kValue);
    s = original;
    InternalKeyComparator::FindShortestSeparator(&s, IKey(limit, 99, ValueType::kValue));
    EXPECT_EQ(s, original) << start << " / " << limit;
  }
}

TEST(DbFormatTest, ShortSuccessor) {
  std::string s = IKey("abc", 7, ValueType::kValue);
  InternalKeyComparator::FindShortSuccessor(&s);
  EXPECT_EQ(s, IKey("b", kMaxSequenceNumber, kValueTypeForSeek));
  const std::string all_ff = IKey("\xff\xff", 7, ValueType::kValue);
  s = all_ff;
  InternalKeyComparator::FindShortSuccessor(&s);
  EXPECT_EQ(s, all_ff);
}

// For random pairs a < b: a <= separator(a, b) < b, and successor(a) >= a.
TEST(DbFormatTest, SeparatorPropertiesHoldForRandomKeys) {
  const InternalKeyComparator cmp;
  uint64_t state = 12345;
  auto next = [&state] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
  };
  for (int i = 0; i < 20000; ++i) {
    std::string a_user(next() % 6, '\0');
    std::string b_user(next() % 6, '\0');
    for (char& c : a_user) c = static_cast<char>(next() % 4 == 0 ? 0xff : next() % 4 + 'a');
    for (char& c : b_user) c = static_cast<char>(next() % 4 == 0 ? 0xff : next() % 4 + 'a');
    std::string a = IKey(a_user, next() % 100, ValueType::kValue);
    std::string b = IKey(b_user, next() % 100, ValueType::kValue);
    if (cmp(a, b) == 0) {
      continue;
    }
    if (cmp(a, b) > 0) {
      std::swap(a, b);
    }
    std::string sep = a;
    InternalKeyComparator::FindShortestSeparator(&sep, b);
    ASSERT_LE(cmp(a, sep), 0);
    ASSERT_LT(cmp(sep, b), 0);
    std::string succ = a;
    InternalKeyComparator::FindShortSuccessor(&succ);
    ASSERT_GE(cmp(succ, a), 0);
  }
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
