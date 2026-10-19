#pragma once

// The internal key format shared by the memtable, WAL replay and SSTables.
//
//   internal_key = user_key || fixed64(sequence << 8 | type)
//
// Internal keys sort by user key ascending (bytewise), then by the packed
// (sequence, type) tag descending, so the newest version of a key comes first.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "util/coding.h"

namespace talus {

using SequenceNumber = uint64_t;

// Sequence numbers occupy 56 bits so they can share a fixed64 with the type.
inline constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

// The highest-valued type. Because tags sort descending, a lookup key built
// with (seq, kValueTypeForSeek) sorts before every entry for the same user key
// with sequence <= seq, so a seek lands on the newest visible version.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

inline constexpr size_t kInternalKeyTagSize = 8;

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  return (seq << 8) | static_cast<uint64_t>(type);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                       ValueType type);

// Returns false if `internal_key` is shorter than the tag or has an unknown
// type byte.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

// Requires internal_key.size() >= kInternalKeyTagSize.
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - kInternalKeyTagSize);
}

inline uint64_t ExtractTag(std::string_view internal_key) {
  return DecodeFixed64(internal_key.data() + internal_key.size() - kInternalKeyTagSize);
}

// A three-way key comparator: negative, zero or positive as a <, ==, > b.
template <typename C>
concept KeyComparator = requires(const C& cmp, std::string_view a, std::string_view b) {
  { cmp(a, b) } -> std::convertible_to<int>;
};

struct InternalKeyComparator {
  int operator()(std::string_view a, std::string_view b) const {
    int r = ExtractUserKey(a).compare(ExtractUserKey(b));
    if (r == 0) {
      const uint64_t a_tag = ExtractTag(a);
      const uint64_t b_tag = ExtractTag(b);
      r = a_tag > b_tag ? -1 : (a_tag < b_tag ? 1 : 0);
    }
    return r;
  }
};

static_assert(KeyComparator<InternalKeyComparator>);

// The internal key used to look up `user_key` as of sequence `seq`. Keys up to
// kInlineSize bytes are built in an inline buffer, so typical lookups do not
// allocate.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber seq);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  std::string_view internal_key() const { return {data_, size_}; }
  std::string_view user_key() const { return {data_, size_ - kInternalKeyTagSize}; }

 private:
  static constexpr size_t kInlineSize = 200;

  const char* data_;
  size_t size_;
  std::unique_ptr<char[]> heap_;  // Only for keys that do not fit inline.
  char inline_[kInlineSize];
};

}  // namespace talus
