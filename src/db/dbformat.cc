#include "db/dbformat.h"

#include <cstring>

namespace talus {

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                       ValueType type) {
  dst->append(user_key);
  PutFixed64(dst, PackSequenceAndType(seq, type));
}

bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < kInternalKeyTagSize) {
    return false;
  }
  const uint64_t tag = ExtractTag(internal_key);
  const auto type = static_cast<uint8_t>(tag & 0xff);
  if (type > static_cast<uint8_t>(ValueType::kValue)) {
    return false;
  }
  result->user_key = ExtractUserKey(internal_key);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return true;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber seq)
    : size_(user_key.size() + kInternalKeyTagSize) {
  char* dst = inline_;
  if (size_ > kInlineSize) {
    heap_ = std::make_unique_for_overwrite<char[]>(size_);
    dst = heap_.get();
  }
  if (!user_key.empty()) {
    std::memcpy(dst, user_key.data(), user_key.size());
  }
  EncodeFixed64(dst + user_key.size(), PackSequenceAndType(seq, kValueTypeForSeek));
  data_ = dst;
}

}  // namespace talus
