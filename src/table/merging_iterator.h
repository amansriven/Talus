#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "table/iterator.h"

namespace talus {

// Merges k sorted child iterators into one sorted stream using a binary
// min-heap: O(log k) per Next(). Entries with equal keys come out in child
// order (lower index first).
//
// If any child reports an error, the merged iterator becomes invalid and
// reports that error. It never silently skips the rest of a failed child,
// which would yield wrong results.
template <KeyComparator Cmp = InternalKeyComparator>
class MergingIterator final : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children, Cmp cmp = Cmp())
      : cmp_(cmp), children_(std::move(children)) {
    heap_.reserve(children_.size());
  }

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    Rebuild();
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    Rebuild();
  }

  void Next() override {
    InternalIterator* top = children_[heap_.front()].get();
    top->Next();
    if (!top->Valid()) {
      if (!top->status().ok()) {
        Fail(top->status());
        return;
      }
      heap_.front() = heap_.back();
      heap_.pop_back();
    }
    if (!heap_.empty()) {
      SiftDown(0);
    }
  }

  std::string_view key() const override { return children_[heap_.front()]->key(); }
  std::string_view value() const override { return children_[heap_.front()]->value(); }
  Status status() const override { return status_; }

 private:
  // True if child a's current entry should come before child b's.
  bool Before(size_t a, size_t b) const {
    const int c = cmp_(children_[a]->key(), children_[b]->key());
    return c < 0 || (c == 0 && a < b);
  }

  void Rebuild() {
    heap_.clear();
    status_ = Status::OK();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) {
        heap_.push_back(i);
      } else if (!children_[i]->status().ok()) {
        Fail(children_[i]->status());
        return;
      }
    }
    for (size_t i = heap_.size() / 2; i-- > 0;) {
      SiftDown(i);
    }
  }

  void SiftDown(size_t pos) {
    const size_t n = heap_.size();
    while (true) {
      size_t best = pos;
      const size_t left = 2 * pos + 1;
      const size_t right = left + 1;
      if (left < n && Before(heap_[left], heap_[best])) {
        best = left;
      }
      if (right < n && Before(heap_[right], heap_[best])) {
        best = right;
      }
      if (best == pos) {
        return;
      }
      std::swap(heap_[pos], heap_[best]);
      pos = best;
    }
  }

  void Fail(const Status& s) {
    status_ = s;
    heap_.clear();
  }

  Cmp cmp_;
  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::vector<size_t> heap_;  // Indexes of valid children, as a min-heap.
  Status status_;
};

}  // namespace talus
