#include "memtable/memtable.h"

#include <cassert>
#include <cstring>

namespace talus {

MemTable::MemTable(bool concurrent_writes, bool use_huge_pages)
    : concurrent_writes_(concurrent_writes),
      arena_(Arena::kDefaultBlockSize, use_huge_pages),
      table_(InternalKeyComparator(), &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  assert(seq <= kMaxSequenceNumber);
  const size_t internal_key_size = key.size() + kInternalKeyTagSize;
  // Encode the internal key and value straight into the node's memory.
  auto fill = [&](char* key_dst, char* value_dst) {
    if (!key.empty()) {
      std::memcpy(key_dst, key.data(), key.size());
    }
    EncodeFixed64(key_dst + key.size(), PackSequenceAndType(seq, type));
    if (!value.empty()) {
      std::memcpy(value_dst, value.data(), value.size());
    }
  };
  if (concurrent_writes_) {
    table_.InsertConcurrently(internal_key_size, value.size(), fill);
  } else {
    table_.Insert(internal_key_size, value.size(), fill);
  }
}

bool MemTable::Get(const LookupKey& key, std::string_view* value, Status* s) const {
  Iterator iter = NewIterator();
  // The lookup key sorts before every entry for the same user key with
  // sequence <= its own, so this lands on the newest visible version, if any.
  iter.Seek(key.internal_key());
  if (!iter.Valid()) {
    return false;
  }
  const std::string_view entry = iter.key();
  if (ExtractUserKey(entry) != key.user_key()) {
    return false;
  }
  switch (static_cast<ValueType>(ExtractTag(entry) & 0xff)) {
    case ValueType::kValue:
      *value = iter.value();
      *s = Status::OK();
      return true;
    case ValueType::kDeletion:
      *s = Status::NotFound();
      return true;
  }
  // Unreachable: Add() only stores the types above.
  assert(false);
  return false;
}

}  // namespace talus
