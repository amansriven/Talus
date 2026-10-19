#include "table/merging_iterator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace talus {
namespace {

struct Bytewise {
  int operator()(std::string_view a, std::string_view b) const { return a.compare(b); }
};

using KVs = std::vector<std::pair<std::string, std::string>>;

// Iterates a sorted vector. If `fail_at` is set, moving onto that index
// makes the iterator invalid with a Corruption status.
class VectorIterator final : public InternalIterator {
 public:
  explicit VectorIterator(KVs kvs, size_t fail_at = SIZE_MAX)
      : kvs_(std::move(kvs)), fail_at_(fail_at) {}
  bool Valid() const override { return pos_ < kvs_.size() && status_.ok(); }
  void SeekToFirst() override { Move(0); }
  void Seek(std::string_view target) override {
    Move(static_cast<size_t>(
        std::lower_bound(kvs_.begin(), kvs_.end(), target,
                         [](const auto& kv, std::string_view t) { return kv.first < t; }) -
        kvs_.begin()));
  }
  void Next() override { Move(pos_ + 1); }
  std::string_view key() const override { return kvs_[pos_].first; }
  std::string_view value() const override { return kvs_[pos_].second; }
  Status status() const override { return status_; }

 private:
  void Move(size_t pos) {
    pos_ = pos;
    if (pos_ >= fail_at_ && pos_ < kvs_.size()) {
      status_ = Status::Corruption("injected");
    }
  }
  KVs kvs_;
  size_t fail_at_;
  size_t pos_ = SIZE_MAX;
  Status status_;
};

using Merger = MergingIterator<Bytewise>;

TEST(MergingIteratorTest, NoChildren) {
  Merger merged({});
  merged.SeekToFirst();
  EXPECT_FALSE(merged.Valid());
  merged.Seek("a");
  EXPECT_FALSE(merged.Valid());
  EXPECT_TRUE(merged.status().ok());
}

TEST(MergingIteratorTest, RandomChildrenMatchSortedUnion) {
  std::mt19937 rng(301);
  for (int trial = 0; trial < 50; ++trial) {
    const int k = 1 + static_cast<int>(rng() % 8);
    std::map<std::string, std::string> model;
    std::vector<std::unique_ptr<InternalIterator>> children;
    for (int c = 0; c < k; ++c) {
      std::map<std::string, std::string> child;
      const int n = static_cast<int>(rng() % 50);  // Some children are empty.
      for (int i = 0; i < n; ++i) {
        // Keys are distinct across children.
        std::string key = "k" + std::to_string(rng() % 1000) + "_" + std::to_string(c);
        child[key] = "v" + key;
      }
      model.insert(child.begin(), child.end());
      children.push_back(std::make_unique<VectorIterator>(KVs(child.begin(), child.end())));
    }
    Merger merged(std::move(children));
    merged.SeekToFirst();
    for (const auto& [key, value] : model) {
      ASSERT_TRUE(merged.Valid());
      EXPECT_EQ(merged.key(), key);
      EXPECT_EQ(merged.value(), value);
      merged.Next();
    }
    EXPECT_FALSE(merged.Valid());

    for (int s = 0; s < 20; ++s) {
      const std::string target = "k" + std::to_string(rng() % 1100);
      merged.Seek(target);
      auto lb = model.lower_bound(target);
      if (lb == model.end()) {
        EXPECT_FALSE(merged.Valid());
      } else {
        ASSERT_TRUE(merged.Valid());
        EXPECT_EQ(merged.key(), lb->first);
      }
    }
  }
}

TEST(MergingIteratorTest, EqualKeysComeOutInChildOrder) {
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(std::make_unique<VectorIterator>(KVs{{"a", "0"}, {"b", "0"}}));
  children.push_back(std::make_unique<VectorIterator>(KVs{{"a", "1"}, {"b", "1"}}));
  Merger merged(std::move(children));
  std::string out;
  for (merged.SeekToFirst(); merged.Valid(); merged.Next()) {
    out += std::string(merged.key()) + std::string(merged.value()) + " ";
  }
  EXPECT_EQ(out, "a0 a1 b0 b1 ");
}

TEST(MergingIteratorTest, ChildErrorStopsTheMerge) {
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(std::make_unique<VectorIterator>(KVs{{"a", ""}, {"c", ""}, {"e", ""}}));
  children.push_back(
      std::make_unique<VectorIterator>(KVs{{"b", ""}, {"d", ""}, {"f", ""}}, /*fail_at=*/1));
  Merger merged(std::move(children));
  std::string seen;
  for (merged.SeekToFirst(); merged.Valid(); merged.Next()) {
    seen += merged.key();
  }
  // "b" is followed by the failing child's error: nothing after it is yielded.
  EXPECT_EQ(seen, "ab");
  EXPECT_TRUE(merged.status().IsCorruption());
}

}  // namespace
}  // namespace talus
