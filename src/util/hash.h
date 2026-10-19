#pragma once

// A from-scratch implementation of the XXH64 algorithm (Yann Collet's
// xxHash64). It is used for bloom filter probes and block-cache shard
// selection. Implementing the published algorithm exactly lets the tests check
// it against the reference test vectors.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace talus {

namespace hash_internal {

inline constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ULL;
inline constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4FULL;
inline constexpr uint64_t kPrime3 = 0x165667B19E3779F9ULL;
inline constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ULL;
inline constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ULL;

inline uint64_t Read64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint32_t Read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t Round(uint64_t acc, uint64_t input) {
  acc += input * kPrime2;
  acc = std::rotl(acc, 31);
  return acc * kPrime1;
}

inline uint64_t MergeRound(uint64_t acc, uint64_t val) {
  acc ^= Round(0, val);
  return acc * kPrime1 + kPrime4;
}

inline uint64_t Avalanche(uint64_t h) {
  h ^= h >> 33;
  h *= kPrime2;
  h ^= h >> 29;
  h *= kPrime3;
  h ^= h >> 32;
  return h;
}

}  // namespace hash_internal

// Returns the XXH64 hash of data[0, n) with the given seed. Assumes a
// little-endian host (checked in util/coding.h).
inline uint64_t Hash64(const char* data, size_t n, uint64_t seed = 0) {
  using namespace hash_internal;  // NOLINT(google-build-using-namespace)
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  const uint8_t* const end = p + n;
  uint64_t h;

  if (n >= 32) {
    uint64_t v1 = seed + kPrime1 + kPrime2;
    uint64_t v2 = seed + kPrime2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - kPrime1;
    const uint8_t* const limit = end - 32;
    do {
      v1 = Round(v1, Read64(p));
      v2 = Round(v2, Read64(p + 8));
      v3 = Round(v3, Read64(p + 16));
      v4 = Round(v4, Read64(p + 24));
      p += 32;
    } while (p <= limit);
    h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    h = MergeRound(h, v1);
    h = MergeRound(h, v2);
    h = MergeRound(h, v3);
    h = MergeRound(h, v4);
  } else {
    h = seed + kPrime5;
  }

  h += static_cast<uint64_t>(n);

  while (end - p >= 8) {
    h ^= Round(0, Read64(p));
    h = std::rotl(h, 27) * kPrime1 + kPrime4;
    p += 8;
  }
  if (end - p >= 4) {
    h ^= static_cast<uint64_t>(Read32(p)) * kPrime1;
    h = std::rotl(h, 23) * kPrime2 + kPrime3;
    p += 4;
  }
  while (p < end) {
    h ^= static_cast<uint64_t>(*p) * kPrime5;
    h = std::rotl(h, 11) * kPrime1;
    ++p;
  }
  return Avalanche(h);
}

inline uint64_t Hash64(std::string_view s, uint64_t seed = 0) {
  return Hash64(s.data(), s.size(), seed);
}

}  // namespace talus
