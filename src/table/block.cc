#include "table/block.h"

#include <utility>

namespace talus {

Block::Block(BlockContents contents) : data_(contents.data), heap_(std::move(contents.heap)) {
  if (data_.size() < sizeof(uint32_t)) {
    return;  // Malformed: no room for num_restarts.
  }
  const uint32_t num_restarts = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  const size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  if (num_restarts == 0 || num_restarts > max_restarts) {
    return;  // Malformed: the restart array does not fit.
  }
  restart_offset_ =
      static_cast<uint32_t>(data_.size() - (1 + size_t{num_restarts}) * sizeof(uint32_t));
  num_restarts_ = num_restarts;
}

}  // namespace talus
