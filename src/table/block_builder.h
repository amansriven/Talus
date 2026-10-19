#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talus {

// Builds a block of prefix-compressed entries:
//
//   entry   := varint32 shared | varint32 non_shared | varint32 value_len
//              | key_delta[non_shared] | value[value_len]
//   trailer := fixed32 restart_offset[num_restarts] | fixed32 num_restarts
//
// Every `restart_interval` entries the full key is stored (shared = 0), and
// the entry's offset is recorded as a restart point.
class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // REQUIRES: Finish() not called since the last Reset(); `key` sorts after
  // every previously added key under the block's comparator.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block. The view stays
  // valid until Reset().
  std::string_view Finish();

  // Clears the builder so it can build another block; keeps its capacity.
  void Reset();

  // The size of the block if Finish() were called now.
  size_t CurrentSizeEstimate() const;
  bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;  // Entries since the last restart point.
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace talus
