#include "db/dbformat.h"

#include <algorithm>
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

void ShortenSeparator(std::string* start, std::string_view limit) {
  const size_t min_len = std::min(start->size(), limit.size());
  size_t diff = 0;
  while (diff < min_len && (*start)[diff] == limit[diff]) {
    ++diff;
  }
  if (diff >= min_len) {
    return;  // One is a prefix of the other; nothing shorter fits.
  }
  const auto byte = static_cast<uint8_t>((*start)[diff]);
  if (byte < 0xff && byte + 1 < static_cast<uint8_t>(limit[diff])) {
    (*start)[diff] = static_cast<char>(byte + 1);
    start->resize(diff + 1);
  }
}

void ShortenSuccessor(std::string* key) {
  for (size_t i = 0; i < key->size(); ++i) {
    const auto byte = static_cast<uint8_t>((*key)[i]);
    if (byte != 0xff) {
      (*key)[i] = static_cast<char>(byte + 1);
      key->resize(i + 1);
      return;
    }
  }
  // All 0xff: leave unchanged.
}

namespace {

// If `user_key` was shortened to something greater, replaces *internal_key
// with it plus the largest tag. Shortened user key u > old user key, so
// (u, max tag) > the old internal key, while staying below the old bound.
void ReplaceIfShortened(std::string* internal_key, std::string_view user_key,
                        std::string* shorter) {
  if (shorter->size() < user_key.size() && user_key.compare(*shorter) < 0) {
    PutFixed64(shorter, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    internal_key->swap(*shorter);
  }
}

}  // namespace

void InternalKeyComparator::FindShortestSeparator(std::string* start, std::string_view limit) {
  const std::string_view user_start = ExtractUserKey(*start);
  std::string shorter(user_start);
  ShortenSeparator(&shorter, ExtractUserKey(limit));
  ReplaceIfShortened(start, user_start, &shorter);
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) {
  const std::string_view user_key = ExtractUserKey(*key);
  std::string shorter(user_key);
  ShortenSuccessor(&shorter);
  ReplaceIfShortened(key, user_key, &shorter);
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
