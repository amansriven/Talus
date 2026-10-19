#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace talus {

// An ordered list of Put and Delete operations that DB::Write applies
// atomically: either every operation becomes visible or none does.
//
// Keys and values must each be smaller than 4 GiB.
class WriteBatch {
 public:
  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);

  // Removes all operations. Keeps the allocated capacity, so a reused batch
  // does not allocate again.
  void Clear();

  // Number of operations.
  uint32_t Count() const;

  // Appends the operations of `source` to this batch.
  void Append(const WriteBatch& source);

  // Size of the serialized batch in bytes.
  size_t ApproximateSize() const { return rep_.size(); }

 private:
  friend class WriteBatchInternal;

  // sequence (fixed64) | count (fixed32) | operations. See docs/DESIGN.md.
  std::string rep_;
};

}  // namespace talus
