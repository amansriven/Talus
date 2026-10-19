#include "util/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace talus {
namespace {

TEST(CodingTest, Fixed32RoundTrip) {
  std::string s;
  for (uint32_t v = 0; v < 100000; ++v) {
    PutFixed32(&s, v);
  }
  std::string_view in(s);
  for (uint32_t v = 0; v < 100000; ++v) {
    uint32_t actual;
    ASSERT_TRUE(GetFixed32(&in, &actual));
    ASSERT_EQ(actual, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, Fixed64RoundTrip) {
  std::string s;
  std::vector<uint64_t> values;
  for (int power = 0; power <= 63; ++power) {
    const uint64_t v = uint64_t{1} << power;
    values.insert(values.end(), {v - 1, v, v + 1});
  }
  for (uint64_t v : values) {
    PutFixed64(&s, v);
  }
  std::string_view in(s);
  for (uint64_t v : values) {
    uint64_t actual;
    ASSERT_TRUE(GetFixed64(&in, &actual));
    ASSERT_EQ(actual, v);
  }
}

TEST(CodingTest, FixedIsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201u);
  PutFixed64(&s, 0x0807060504030201ull);
  ASSERT_EQ(s.size(), 12u);
  EXPECT_EQ(s.substr(0, 4), std::string("\x01\x02\x03\x04", 4));
  EXPECT_EQ(s.substr(4), std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(CodingTest, Varint32RoundTrip) {
  std::string s;
  for (uint32_t i = 0; i < 32 * 32; ++i) {
    PutVarint32(&s, (i / 32) << (i % 32));
  }
  std::string_view in(s);
  for (uint32_t i = 0; i < 32 * 32; ++i) {
    const uint32_t expected = (i / 32) << (i % 32);
    const size_t before = in.size();
    uint32_t actual;
    ASSERT_TRUE(GetVarint32(&in, &actual));
    ASSERT_EQ(actual, expected);
    ASSERT_EQ(static_cast<size_t>(VarintLength(actual)), before - in.size());
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, Varint64RoundTrip) {
  std::vector<uint64_t> values = {0, 100, ~uint64_t{0}, ~uint64_t{0} - 1};
  for (int k = 0; k < 64; ++k) {
    const uint64_t power = uint64_t{1} << k;
    values.insert(values.end(), {power, power - 1, power + 1});
  }
  std::string s;
  for (uint64_t v : values) {
    PutVarint64(&s, v);
  }
  std::string_view in(s);
  for (uint64_t v : values) {
    const size_t before = in.size();
    uint64_t actual;
    ASSERT_TRUE(GetVarint64(&in, &actual));
    ASSERT_EQ(actual, v);
    ASSERT_EQ(static_cast<size_t>(VarintLength(actual)), before - in.size());
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, VarintMaxLengths) {
  EXPECT_EQ(VarintLength(std::numeric_limits<uint32_t>::max()), kMaxVarint32Bytes);
  EXPECT_EQ(VarintLength(std::numeric_limits<uint64_t>::max()), kMaxVarint64Bytes);
}

TEST(CodingTest, Varint32RejectsOverflow) {
  // Five bytes whose last byte sets bits above bit 31.
  const std::string too_big("\x81\x82\x83\x84\x85\x11", 6);
  uint32_t v;
  EXPECT_EQ(GetVarint32Ptr(too_big.data(), too_big.data() + too_big.size(), &v), nullptr);

  // Six continuation bytes.
  const std::string too_long("\xff\xff\xff\xff\xff\x01", 6);
  EXPECT_EQ(GetVarint32Ptr(too_long.data(), too_long.data() + too_long.size(), &v), nullptr);
}

TEST(CodingTest, Varint64RejectsOverflow) {
  std::string too_big(9, '\xff');
  too_big.push_back('\x02');  // Tenth byte may only carry bit 63.
  uint64_t v;
  EXPECT_EQ(GetVarint64Ptr(too_big.data(), too_big.data() + too_big.size(), &v), nullptr);

  std::string max(9, '\xff');
  max.push_back('\x01');
  ASSERT_NE(GetVarint64Ptr(max.data(), max.data() + max.size(), &v), nullptr);
  EXPECT_EQ(v, std::numeric_limits<uint64_t>::max());
}

TEST(CodingTest, VarintRejectsTruncation) {
  std::string s;
  PutVarint32(&s, std::numeric_limits<uint32_t>::max());
  PutVarint64(&s, std::numeric_limits<uint64_t>::max());
  const char* begin = s.data();
  for (size_t len = 0; len < kMaxVarint32Bytes; ++len) {
    uint32_t v;
    EXPECT_EQ(GetVarint32Ptr(begin, begin + len, &v), nullptr) << len;
  }
  const char* begin64 = begin + kMaxVarint32Bytes;
  for (size_t len = 0; len < kMaxVarint64Bytes; ++len) {
    uint64_t v;
    EXPECT_EQ(GetVarint64Ptr(begin64, begin64 + len, &v), nullptr) << len;
  }
}

TEST(CodingTest, LengthPrefixed) {
  std::string s;
  PutLengthPrefixed(&s, "");
  PutLengthPrefixed(&s, "foo");
  PutLengthPrefixed(&s, std::string(200, 'x'));
  std::string_view in(s);
  std::string_view v;
  ASSERT_TRUE(GetLengthPrefixed(&in, &v));
  EXPECT_EQ(v, "");
  ASSERT_TRUE(GetLengthPrefixed(&in, &v));
  EXPECT_EQ(v, "foo");
  ASSERT_TRUE(GetLengthPrefixed(&in, &v));
  EXPECT_EQ(v, std::string(200, 'x'));
  EXPECT_FALSE(GetLengthPrefixed(&in, &v));
}

TEST(CodingTest, LengthPrefixedRejectsShortPayload) {
  std::string s;
  PutLengthPrefixed(&s, "hello");
  s.pop_back();
  std::string_view in(s);
  std::string_view v;
  EXPECT_FALSE(GetLengthPrefixed(&in, &v));
}

}  // namespace
}  // namespace talus
