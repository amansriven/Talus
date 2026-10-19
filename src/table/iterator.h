#pragma once

#include <concepts>
#include <string_view>

#include "Talus/status.h"

namespace talus {

// The forward-iterator interface over internal keys. Concrete iterators
// (blocks, skiplists) satisfy it statically and are used directly where the
// type is known.
template <typename I>
concept ForwardIterator = requires(I& it, const I& cit, std::string_view target) {
  { cit.Valid() } -> std::same_as<bool>;
  it.SeekToFirst();
  it.Seek(target);  // First entry with key >= target.
  it.Next();
  { cit.key() } -> std::convertible_to<std::string_view>;
  { cit.value() } -> std::convertible_to<std::string_view>;
  { cit.status() } -> std::convertible_to<Status>;
};

// A type-erased ForwardIterator, for merging iterators of different kinds
// (memtable, L0 tables, levels). Virtual dispatch is paid only where the
// children are heterogeneous.
class InternalIterator {
 public:
  virtual ~InternalIterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  // REQUIRES: Valid(). The views stay valid until the next move.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK once an error is hit. An iterator in error is not Valid().
  virtual Status status() const = 0;
};

static_assert(ForwardIterator<InternalIterator>);

}  // namespace talus
