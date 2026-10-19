#include "table/filter_policy.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>

namespace talus {
namespace {

constexpr size_t kTrailerSize = 2;
constexpr int kMaxProbes = 30;

__extension__ typedef unsigned __int128 uint128_t;  // NOLINT(modernize-use-using)

// Maps a uniform 64-bit value onto [0, n) without division (Lemire's
// multiply-shift "fastrange").
inline uint64_t FastRange64(uint64_t x, uint64_t n) {
  return static_cast<uint64_t>((static_cast<uint128_t>(x) * n) >> 64);
}
inline uint32_t FastRange32(uint32_t x, uint32_t n) {
  return static_cast<uint32_t>((static_cast<uint64_t>(x) * n) >> 32);
}

void AppendTrailer(std::string* out, int num_probes, FilterType type) {
  out->push_back(static_cast<char>(num_probes));
  out->push_back(static_cast<char>(type));
}

int ProbesOf(std::string_view filter) { return static_cast<uint8_t>(filter[filter.size() - 2]); }

// Poisson-weighted false-positive rate of a blocked filter. With
// lambda = 512 / bits_per_key keys per 512-bit block on average, a block
// holding j keys has each bit set with probability 1 - (1 - 1/512)^(k*j), and
// a non-member is a false positive if all k of its probes hit set bits.
double BlockedFpr(int bits_per_key, int k) {
  constexpr double kBits = BlockedBloomPolicy::kBlockBytes * 8;
  const double lambda = kBits / bits_per_key;
  const int max_j = static_cast<int>(lambda + 12 * std::sqrt(lambda) + 32);
  double p_j = std::exp(-lambda);  // Poisson(j = 0)
  double fpr = 0;
  for (int j = 0; j <= max_j; ++j) {
    const double bit_set = 1 - std::pow(1 - 1 / kBits, static_cast<double>(k) * j);
    fpr += p_j * std::pow(bit_set, k);
    p_j *= lambda / (j + 1);
  }
  return fpr;
}

double StandardFpr(int bits_per_key, int k) {
  return std::pow(1 - std::exp(-static_cast<double>(k) / bits_per_key), k);
}

}  // namespace

int NumProbes(FilterType type, int bits_per_key) {
  bits_per_key = std::max(bits_per_key, 1);
  if (type == FilterType::kStandardBloom) {
    // k = m/n * ln 2 minimizes the standard filter's false-positive rate.
    return std::clamp(static_cast<int>(std::lround(bits_per_key * 0.69314718)), 1, kMaxProbes);
  }
  // A blocked filter's best k is lower, because blocks with more keys than
  // average saturate. Pick it by minimizing the model.
  int best = 1;
  for (int k = 2; k <= kMaxProbes; ++k) {
    if (BlockedFpr(bits_per_key, k) < BlockedFpr(bits_per_key, best)) {
      best = k;
    }
  }
  return best;
}

double TheoreticalFalsePositiveRate(FilterType type, int bits_per_key, int num_probes) {
  switch (type) {
    case FilterType::kStandardBloom:
      return StandardFpr(bits_per_key, num_probes);
    case FilterType::kBlockedBloom:
      return BlockedFpr(bits_per_key, num_probes);
    case FilterType::kNone:
      break;
  }
  return 1.0;
}

// --- Standard bloom -------------------------------------------------------
//
// filter := bits[m / 8] | num_probes | kStandardBloom,  m a multiple of 64.
// Probe i tests bit FastRange64(h + i * delta, m), with delta = rotl(h, 21)
// forced odd (Kirsch-Mitzenmacher double hashing).

void StandardBloomPolicy::Build(int bits_per_key, std::span<const uint64_t> hashes,
                                std::string* out) {
  const int k = NumProbes(kType, bits_per_key);
  uint64_t bits =
      std::max<uint64_t>(64, hashes.size() * static_cast<uint64_t>(std::max(bits_per_key, 1)));
  bits = (bits + 63) / 64 * 64;
  out->assign(bits / 8, '\0');
  auto* array = reinterpret_cast<uint8_t*>(out->data());
  for (uint64_t h : hashes) {
    const uint64_t delta = std::rotl(h, 21) | 1;
    for (int i = 0; i < k; ++i) {
      const uint64_t bit = FastRange64(h, bits);
      array[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
      h += delta;
    }
  }
  AppendTrailer(out, k, kType);
}

bool StandardBloomPolicy::MayContain(std::string_view filter, uint64_t hash) {
  const uint64_t bits = (filter.size() - kTrailerSize) * 8;
  const int k = ProbesOf(filter);
  const auto* array = reinterpret_cast<const uint8_t*>(filter.data());
  const uint64_t delta = std::rotl(hash, 21) | 1;
  for (int i = 0; i < k; ++i) {
    const uint64_t bit = FastRange64(hash, bits);
    if ((array[bit >> 3] & (1u << (bit & 7))) == 0) {
      return false;
    }
    hash += delta;
  }
  return true;
}

bool StandardBloomPolicy::Validate(std::string_view filter) {
  if (filter.size() < kTrailerSize + 8 || (filter.size() - kTrailerSize) % 8 != 0) {
    return false;
  }
  const int k = ProbesOf(filter);
  return k >= 1 && k <= kMaxProbes;
}

// --- Cache-line-blocked bloom -----------------------------------------------
//
// filter := block[num_blocks][64 bytes] | num_probes | kBlockedBloom
// The high 32 hash bits pick the block (FastRange32). The low 32 bits h2
// supply the probes: probe i tests bit (h2 >> 23) of the block, a value in
// [0, 512), then sets h2 *= 0x9e3779b9 (the 32-bit golden ratio) to remix the
// bits for the next probe.

void BlockedBloomPolicy::Build(int bits_per_key, std::span<const uint64_t> hashes,
                               std::string* out) {
  const int k = NumProbes(kType, bits_per_key);
  const uint64_t total_bits = hashes.size() * static_cast<uint64_t>(std::max(bits_per_key, 1));
  const auto num_blocks = static_cast<uint32_t>(
      std::max<uint64_t>(1, (total_bits + kBlockBytes * 8 - 1) / (kBlockBytes * 8)));
  out->assign(size_t{num_blocks} * kBlockBytes, '\0');
  auto* array = reinterpret_cast<uint8_t*>(out->data());
  for (uint64_t h : hashes) {
    uint8_t* block =
        array + size_t{FastRange32(static_cast<uint32_t>(h >> 32), num_blocks)} * kBlockBytes;
    auto h2 = static_cast<uint32_t>(h);
    for (int i = 0; i < k; ++i) {
      const uint32_t bit = h2 >> 23;
      block[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
      h2 *= 0x9e3779b9u;
    }
  }
  AppendTrailer(out, k, kType);
}

bool BlockedBloomPolicy::MayContain(std::string_view filter, uint64_t hash) {
  const auto num_blocks = static_cast<uint32_t>((filter.size() - kTrailerSize) / kBlockBytes);
  const int k = ProbesOf(filter);
  const auto* block =
      reinterpret_cast<const uint8_t*>(filter.data()) +
      size_t{FastRange32(static_cast<uint32_t>(hash >> 32), num_blocks)} * kBlockBytes;
  auto h2 = static_cast<uint32_t>(hash);
  for (int i = 0; i < k; ++i) {
    const uint32_t bit = h2 >> 23;
    if ((block[bit >> 3] & (1u << (bit & 7))) == 0) {
      return false;
    }
    h2 *= 0x9e3779b9u;
  }
  return true;
}

bool BlockedBloomPolicy::Validate(std::string_view filter) {
  if (filter.size() < kTrailerSize + kBlockBytes ||
      (filter.size() - kTrailerSize) % kBlockBytes != 0 ||
      (filter.size() - kTrailerSize) / kBlockBytes > UINT32_MAX) {
    return false;
  }
  const int k = ProbesOf(filter);
  return k >= 1 && k <= kMaxProbes;
}

// --- Runtime dispatch ---------------------------------------------------------

void BuildFilter(FilterType type, int bits_per_key, std::span<const uint64_t> hashes,
                 std::string* out) {
  switch (type) {
    case FilterType::kStandardBloom:
      StandardBloomPolicy::Build(bits_per_key, hashes, out);
      return;
    case FilterType::kBlockedBloom:
      BlockedBloomPolicy::Build(bits_per_key, hashes, out);
      return;
    case FilterType::kNone:
      break;
  }
  assert(false);
}

bool FilterMayContain(std::string_view filter, uint64_t hash) {
  switch (static_cast<FilterType>(filter.back())) {
    case FilterType::kBlockedBloom:
      return BlockedBloomPolicy::MayContain(filter, hash);
    case FilterType::kStandardBloom:
      return StandardBloomPolicy::MayContain(filter, hash);
    case FilterType::kNone:
      break;
  }
  return true;  // Unreachable for validated filters; "may contain" is the safe answer.
}

Status ValidateFilter(std::string_view filter) {
  if (filter.size() < kTrailerSize) {
    return Status::Corruption("filter too short");
  }
  bool ok = false;
  switch (static_cast<FilterType>(filter.back())) {
    case FilterType::kBlockedBloom:
      ok = BlockedBloomPolicy::Validate(filter);
      break;
    case FilterType::kStandardBloom:
      ok = StandardBloomPolicy::Validate(filter);
      break;
    case FilterType::kNone:
      break;
  }
  return ok ? Status::OK() : Status::Corruption("malformed or unknown filter");
}

}  // namespace talus
