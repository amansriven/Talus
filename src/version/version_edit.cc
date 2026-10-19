#include "version/version_edit.h"

#include "util/coding.h"

namespace talus {
namespace {

enum Tag : uint32_t {
  kLogNumber = 1,
  kNextFileNumber = 2,
  kLastSequence = 3,
  kCompactPointer = 4,
  kDeletedFile = 5,
  kNewFile = 6,
};

bool GetLevel(std::string_view* input, int* level) {
  uint32_t v = 0;
  if (!GetVarint32(input, &v) || v >= VersionEdit::kMaxLevels) {
    return false;
  }
  *level = static_cast<int>(v);
  return true;
}

bool GetInternalKey(std::string_view* input, std::string* key) {
  std::string_view k;
  if (!GetLengthPrefixed(input, &k) || k.size() < kInternalKeyTagSize) {
    return false;
  }
  key->assign(k);
  return true;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, *last_sequence);
  }
  for (const auto& [level, key] : compact_pointers) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixed(dst, key);
  }
  for (const auto& [level, number] : deleted_files) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  *this = VersionEdit();
  std::string_view input = src;
  while (!input.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) {
      return Status::Corruption("VersionEdit", "bad tag");
    }
    bool ok = false;
    switch (tag) {
      case kLogNumber: {
        uint64_t v = 0;
        ok = GetVarint64(&input, &v);
        log_number = v;
        break;
      }
      case kNextFileNumber: {
        uint64_t v = 0;
        ok = GetVarint64(&input, &v);
        next_file_number = v;
        break;
      }
      case kLastSequence: {
        uint64_t v = 0;
        ok = GetVarint64(&input, &v) && v <= kMaxSequenceNumber;
        last_sequence = v;
        break;
      }
      case kCompactPointer: {
        int level = 0;
        std::string key;
        ok = GetLevel(&input, &level) && GetInternalKey(&input, &key);
        if (ok) {
          compact_pointers.emplace_back(level, std::move(key));
        }
        break;
      }
      case kDeletedFile: {
        int level = 0;
        uint64_t number = 0;
        ok = GetLevel(&input, &level) && GetVarint64(&input, &number);
        if (ok) {
          deleted_files.emplace(level, number);
        }
        break;
      }
      case kNewFile: {
        int level = 0;
        FileMetaData f;
        ok = GetLevel(&input, &level) && GetVarint64(&input, &f.number) &&
             GetVarint64(&input, &f.file_size) && GetInternalKey(&input, &f.smallest) &&
             GetInternalKey(&input, &f.largest);
        if (ok) {
          new_files.emplace_back(level, std::move(f));
        }
        break;
      }
      default:
        return Status::Corruption("VersionEdit", "unknown tag");
    }
    if (!ok) {
      return Status::Corruption("VersionEdit", "truncated or invalid field");
    }
  }
  return Status::OK();
}

}  // namespace talus
