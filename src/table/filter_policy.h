#pragma once

// Bloom filters for SSTables. A table has one filter over the 64-bit hashes
// (Hash64) of all its user keys.
//
// Both filter kinds end with a 2-byte trailer:
//   filter := bits... | num_probes (1B) | FilterType (1B)
// so a reader can probe any filter without knowing in advance which policy
// built it. Dispatch is a switch on that byte, not a virtual call.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "Talus/status.h"

namespace talus {

enum class FilterType : uint8_t {
  kNone = 0,
  kStandardBloom = 1,
  kBlockedBloom = 2,
};

// A filter policy builds a filter from key hashes and probes it. Policies
// are stateless and statically dispatched.
template <typename P>
concept FilterPolicy = requires(int bits_per_key, std::span<const uint64_t> hashes,
                                std::string* out, std::string_view filter, uint64_t hash) {
  { P::kType } -> std::convertible_to<FilterType>;
  P::Build(bits_per_key, hashes, out);  // Replaces *out with the filter.
  { P::MayContain(filter, hash) } -> std::same_as<bool>;
  { P::Validate(filter) } -> std::same_as<bool>;
};

// The classic bloom filter: m = n * bits_per_key bits, and k probes spread
// over all of them with double hashing. A probe touches up to k cache lines.
struct StandardBloomPolicy {
  static constexpr FilterType kType = FilterType::kStandardBloom;
  static void Build(int bits_per_key, std::span<const uint64_t> hashes, std::string* out);
  static bool MayContain(std::string_view filter, uint64_t hash);
  static bool Validate(std::string_view filter);
};

// A cache-line-blocked bloom filter: the bits form 64-byte blocks. A key's
// hash picks one block, and all k probes fall inside it, so a lookup costs
// at most one cache miss. If the filter's memory is 64-byte aligned, each
// block is exactly one cache line.
struct BlockedBloomPolicy {
  static constexpr FilterType kType = FilterType::kBlockedBloom;
  static constexpr size_t kBlockBytes = 64;
  static void Build(int bits_per_key, std::span<const uint64_t> hashes, std::string* out);
  static bool MayContain(std::string_view filter, uint64_t hash);
  static bool Validate(std::string_view filter);
};

static_assert(FilterPolicy<StandardBloomPolicy>);
static_assert(FilterPolicy<BlockedBloomPolicy>);

// Builds a filter of the given type. REQUIRES: type != kNone.
void BuildFilter(FilterType type, int bits_per_key, std::span<const uint64_t> hashes,
                 std::string* out);

// Probes a filter built by BuildFilter(). REQUIRES: ValidateFilter(filter).
bool FilterMayContain(std::string_view filter, uint64_t hash);

// Checks a filter's trailer and size. A malformed filter could produce false
// negatives (wrong NotFound results), so tables refuse to open with one.
Status ValidateFilter(std::string_view filter);

// The probe count each policy uses for a given bits_per_key.
int NumProbes(FilterType type, int bits_per_key);

// The expected false-positive rate for a large number of keys, from the
// policy's mathematical model. Used to pick probe counts and to test the
// implementations.
double TheoreticalFalsePositiveRate(FilterType type, int bits_per_key, int num_probes);

}  // namespace talus
