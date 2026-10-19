#pragma once

#include <string_view>

#include "Talus/status.h"

namespace talus {

// A forward iterator over a consistent snapshot of a DB's user keys, in
// bytewise order. The key and value views stay valid until the iterator
// moves or is destroyed. An iterator must be destroyed before its DB.
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first key >= target.
  virtual void Seek(std::string_view target) = 0;
  // REQUIRES: Valid().
  virtual void Next() = 0;
  // REQUIRES: Valid().
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK if an error (e.g. a corrupt block) was hit. The iterator is then
  // not Valid(): it never silently skips data it could not read.
  virtual Status status() const = 0;
};

}  // namespace talus
