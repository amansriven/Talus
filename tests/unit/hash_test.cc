#include "util/hash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

namespace talus {
namespace {

struct Vector {
  std::string input;
  uint64_t seed;
  uint64_t expected;
};

std::string Bytes1027() {
  std::string s;
  for (int rep = 0; rep < 4; ++rep) {
    for (int b = 0; b < 256; ++b) {
      s.push_back(static_cast<char>(b));
    }
  }
  return s + "xyz";
}

// Reference values from the official xxHash implementation (python-xxhash).
// The lengths cover each code path: empty, 1/4/8-byte tails, just below and at
// the 32-byte stripe threshold, and a multi-stripe input with a ragged tail.
TEST(HashTest, MatchesXxh64ReferenceVectors) {
  constexpr uint64_t kSeed = 0x9747b28c;
  const Vector vectors[] = {
      {"", 0, 0xef46db3751d8e999ULL},
      {"", kSeed, 0x495a197c8d074e3dULL},
      {"a", 0, 0xd24ec4f1a98c6e5bULL},
      {"a", kSeed, 0x0c7350bffbfe1681ULL},
      {"abc", 0, 0x44bc2cf5ad770999ULL},
      {"abc", kSeed, 0x7d79a0222a9406c7ULL},
      {"abcd", 0, 0xde0327b0d25d92ccULL},
      {"abcd", kSeed, 0xcdb94fd287d80311ULL},
      {"0123456", 0, 0x97ee4fe4a0ff4dfaULL},
      {"0123456", kSeed, 0xdc0f0f3092f005b9ULL},
      {"01234567", 0, 0xe4ba22a49ad89d3fULL},
      {"01234567", kSeed, 0x0b9e41f74f139234ULL},
      {"hello world", 0, 0x45ab6734b21e6968ULL},
      {"hello world", kSeed, 0xa4c61180134982cbULL},
      {"0123456789abcdefghijklmnopqrstu", 0, 0x80adfc1d42020f39ULL},
      {"0123456789abcdefghijklmnopqrstu", kSeed, 0xf358085702c0a740ULL},
      {"0123456789abcdefghijklmnopqrstuv", 0, 0xbf7c9dbe16b5c6e2ULL},
      {"0123456789abcdefghijklmnopqrstuv", kSeed, 0x1aa00cc3141e7efdULL},
      {"The quick brown fox jumps over the lazy dog", 0, 0x0b242d361fda71bcULL},
      {"The quick brown fox jumps over the lazy dog", kSeed, 0x3e182f2bc9dbde4bULL},
      {Bytes1027(), 0, 0xe146cb31b65bc21aULL},
      {Bytes1027(), kSeed, 0xe3f37d95f816874fULL},
  };
  for (const Vector& v : vectors) {
    EXPECT_EQ(Hash64(v.input, v.seed), v.expected)
        << "len=" << v.input.size() << " seed=" << v.seed;
  }
}

TEST(HashTest, UnalignedInputGivesSameResult) {
  const std::string base = "0123456789abcdefghijklmnopqrstuvwxyz0123456789";
  for (size_t offset = 0; offset < 8; ++offset) {
    std::string padded = std::string(offset, '#') + base;
    EXPECT_EQ(Hash64(padded.data() + offset, base.size()), Hash64(base));
  }
}

TEST(HashTest, NoCollisionsOnSequentialKeys) {
  std::set<uint64_t> seen;
  for (int i = 0; i < 100000; ++i) {
    ASSERT_TRUE(seen.insert(Hash64("key" + std::to_string(i))).second) << i;
  }
}

}  // namespace
}  // namespace talus
