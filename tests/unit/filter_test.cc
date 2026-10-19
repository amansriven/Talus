#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "table/filter_policy.h"
#include "util/hash.h"

namespace talus {
namespace {

std::vector<uint64_t> Hashes(const std::string& prefix, int n) {
  std::vector<uint64_t> hashes;
  hashes.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    hashes.push_back(Hash64(prefix + std::to_string(i)));
  }
  return hashes;
}

class FilterTest : public ::testing::TestWithParam<FilterType> {};

INSTANTIATE_TEST_SUITE_P(Policies, FilterTest,
                         ::testing::Values(FilterType::kStandardBloom, FilterType::kBlockedBloom),
                         [](const auto& info) {
                           return info.param == FilterType::kBlockedBloom ? "Blocked" : "Standard";
                         });

TEST_P(FilterTest, NoFalseNegatives) {
  for (int n : {0, 1, 10, 100, 1000, 10000}) {
    const auto keys = Hashes("key", n);
    std::string filter;
    BuildFilter(GetParam(), 10, keys, &filter);
    ASSERT_TRUE(ValidateFilter(filter).ok()) << n;
    for (uint64_t h : keys) {
      ASSERT_TRUE(FilterMayContain(filter, h)) << "false negative with n=" << n;
    }
  }
}

// The spec requires the measured false-positive rate at 10 bits/key to be
// within 20% of the theoretical rate.
TEST_P(FilterTest, FalsePositiveRateMatchesTheoryAtTenBitsPerKey) {
  constexpr int kBitsPerKey = 10;
  constexpr int kKeys = 100'000;
  constexpr int kQueries = 1'000'000;
  std::string filter;
  BuildFilter(GetParam(), kBitsPerKey, Hashes("member", kKeys), &filter);

  int false_positives = 0;
  for (int i = 0; i < kQueries; ++i) {
    false_positives += FilterMayContain(filter, Hash64("absent" + std::to_string(i))) ? 1 : 0;
  }
  const double measured = static_cast<double>(false_positives) / kQueries;
  const double theory =
      TheoreticalFalsePositiveRate(GetParam(), kBitsPerKey, NumProbes(GetParam(), kBitsPerKey));
  RecordProperty("measured_fpr", std::to_string(measured));
  RecordProperty("theoretical_fpr", std::to_string(theory));
  EXPECT_NEAR(measured, theory, 0.2 * theory) << "measured " << measured << " vs theory " << theory;
}

TEST_P(FilterTest, ValidateRejectsMalformedFilters) {
  std::string filter;
  BuildFilter(GetParam(), 10, Hashes("k", 1000), &filter);
  ASSERT_TRUE(ValidateFilter(filter).ok());

  EXPECT_TRUE(ValidateFilter("").IsCorruption());
  std::string unknown_type = filter;
  unknown_type.back() = 9;
  EXPECT_TRUE(ValidateFilter(unknown_type).IsCorruption());
  std::string zero_probes = filter;
  zero_probes[zero_probes.size() - 2] = 0;
  EXPECT_TRUE(ValidateFilter(zero_probes).IsCorruption());
  std::string bad_size = filter;
  bad_size.insert(bad_size.begin(), 'x');  // Body no longer a whole number of words/blocks.
  EXPECT_TRUE(ValidateFilter(bad_size).IsCorruption());
}

TEST(FilterModelTest, ProbeCountsAndTheory) {
  EXPECT_EQ(NumProbes(FilterType::kStandardBloom, 10), 7);
  // Blocked filters saturate unevenly, so their best k is no higher, and their
  // false-positive rate is somewhat worse than a standard filter's.
  EXPECT_LE(NumProbes(FilterType::kBlockedBloom, 10), 7);
  const double standard = TheoreticalFalsePositiveRate(FilterType::kStandardBloom, 10, 7);
  const double blocked = TheoreticalFalsePositiveRate(FilterType::kBlockedBloom, 10,
                                                      NumProbes(FilterType::kBlockedBloom, 10));
  EXPECT_NEAR(standard, 0.0082, 0.0002);
  EXPECT_GT(blocked, standard);
  EXPECT_LT(blocked, 2 * standard);
}

}  // namespace
}  // namespace talus
