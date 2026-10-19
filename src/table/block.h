#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "Talus/status.h"
#include "db/dbformat.h"
#include "table/format.h"
#include "util/coding.h"

namespace talus {

// A parsed block (see BlockBuilder for the format). Immutable; read it with
// a BlockIter.
class Block {
 public:
  Block() = default;
  explicit Block(BlockContents contents);

  Block(Block&&) noexcept = default;
  Block& operator=(Block&&) noexcept = default;
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }
  // False if the restart array is inconsistent with the block size.
  bool well_formed() const { return num_restarts_ > 0; }

 private:
  template <KeyComparator Cmp>
  friend class BlockIter;

  std::string_view data_;
  std::unique_ptr<char[]> heap_;  // Owns data_ unless it points into an mmap.
  uint32_t restart_offset_ = 0;   // Offset of the restart array.
  uint32_t num_restarts_ = 0;     // 0 if malformed.
};

// A key buffer that holds keys up to kInline bytes without allocating, so
// iterating a block (and the Get path on a cached block) is allocation-free
// for typical keys. Longer keys spill to the heap.
class KeyBuffer {
 public:
  KeyBuffer() = default;
  KeyBuffer(const KeyBuffer&) = delete;
  KeyBuffer& operator=(const KeyBuffer&) = delete;

  std::string_view view() const { return {data_, size_}; }
  size_t size() const { return size_; }
  void Truncate(size_t n) { size_ = n; }
  void Append(const char* p, size_t n) {
    if (size_ + n > capacity_) {
      Grow(size_ + n);
    }
    std::memcpy(data_ + size_, p, n);
    size_ += n;
  }

 private:
  static constexpr size_t kInline = 128;

  void Grow(size_t needed) {
    const size_t capacity = std::max(needed, 2 * capacity_);
    if (data_ == inline_) {
      heap_.assign(inline_, size_);
    }
    heap_.resize(capacity);
    data_ = heap_.data();
    capacity_ = capacity;
  }

  char inline_[kInline];
  std::string heap_;
  char* data_ = inline_;
  size_t size_ = 0;
  size_t capacity_ = kInline;
};

// A forward iterator over a Block, valid while the Block lives. Seek()
// binary-searches the restart points, then scans linearly.
//
// Malformed input (bad varints, out-of-range lengths or restart offsets, a
// restart entry with shared != 0, keys shorter than Cmp::kMinKeySize) makes
// the iterator invalid with a Corruption status. It never reads outside the
// block.
template <KeyComparator Cmp = InternalKeyComparator>
class BlockIter {
 public:
  BlockIter() = default;
  explicit BlockIter(const Block& block, Cmp cmp = Cmp()) : cmp_(cmp) { Reset(block); }

  BlockIter(const BlockIter&) = delete;
  BlockIter& operator=(const BlockIter&) = delete;

  // Points this iterator at another block; the result is not Valid() until a
  // Seek.
  void Reset(const Block& block) {
    data_ = block.data_.data();
    restarts_ = block.restart_offset_;
    num_restarts_ = block.num_restarts_;
    current_ = restarts_;
    restart_index_ = num_restarts_;
    key_.Truncate(0);
    value_ = {};
    status_ = block.well_formed() ? Status::OK() : Status::Corruption("malformed block");
  }

  bool Valid() const { return current_ < restarts_; }
  const Status& status() const { return status_; }
  std::string_view key() const { return key_.view(); }
  std::string_view value() const { return value_; }

  void SeekToFirst() {
    if (num_restarts_ == 0) {
      return;
    }
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void Next() { ParseNextKey(); }

  // Positions at the first entry with key >= target.
  void Seek(std::string_view target) {
    if (num_restarts_ == 0) {
      return;
    }
    // Find the last restart point whose key is < target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t offset = RestartPoint(mid);
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = offset < restarts_
                                ? DecodeEntry(data_ + offset, &shared, &non_shared, &value_length)
                                : nullptr;
      if (key_ptr == nullptr || shared != 0 || non_shared < MinKeySize()) {
        CorruptionError();
        return;
      }
      if (cmp_(std::string_view(key_ptr, non_shared), target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    SeekToRestartPoint(left);
    while (ParseNextKey()) {
      if (cmp_(key_.view(), target) >= 0) {
        return;
      }
    }
  }

 private:
  static constexpr size_t MinKeySize() {
    if constexpr (requires { Cmp::kMinKeySize; }) {
      return Cmp::kMinKeySize;
    } else {
      return 0;
    }
  }

  uint32_t RestartPoint(uint32_t index) const {
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    const uint32_t offset = RestartPoint(index);
    if (offset > restarts_) {
      CorruptionError();
      return;
    }
    key_.Truncate(0);
    restart_index_ = index;
    // ParseNextKey() starts at the end of value_.
    value_ = std::string_view(data_ + offset, 0);
  }

  // Decodes the entry header at p. Returns a pointer to the key delta, or
  // nullptr if the header or the lengths it declares overrun the entries.
  const char* DecodeEntry(const char* p, uint32_t* shared, uint32_t* non_shared,
                          uint32_t* value_length) const {
    const char* limit = data_ + restarts_;
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr ||
        (p = GetVarint32Ptr(p, limit, non_shared)) == nullptr ||
        (p = GetVarint32Ptr(p, limit, value_length)) == nullptr) {
      return nullptr;
    }
    if (uint64_t{*non_shared} + *value_length > static_cast<uint64_t>(limit - p)) {
      return nullptr;
    }
    return p;
  }

  bool ParseNextKey() {
    if (value_.data() == nullptr) {
      return false;  // Already invalid (corruption or never positioned).
    }
    current_ = static_cast<uint32_t>(value_.data() + value_.size() - data_);
    if (current_ >= restarts_) {
      current_ = restarts_;  // End of block.
      restart_index_ = num_restarts_;
      return false;
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    const char* p = DecodeEntry(data_ + current_, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared || shared + non_shared < MinKeySize()) {
      CorruptionError();
      return false;
    }
    key_.Truncate(shared);
    key_.Append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ && RestartPoint(restart_index_ + 1) <= current_) {
      ++restart_index_;
    }
    return true;
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    key_.Truncate(0);
    value_ = {};
    status_ = Status::Corruption("bad entry in block");
  }

  Cmp cmp_{};
  const char* data_ = nullptr;
  uint32_t restarts_ = 0;  // Offset of the restart array; entries end here.
  uint32_t num_restarts_ = 0;
  uint32_t current_ = 0;        // Offset of the current entry; >= restarts_ if invalid.
  uint32_t restart_index_ = 0;  // Restart region containing current_.
  KeyBuffer key_;
  std::string_view value_;
  Status status_;
};

}  // namespace talus
