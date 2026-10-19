#include "Talus/write_batch.h"

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace talus {

WriteBatch::WriteBatch() : rep_(WriteBatchInternal::kHeaderSize, '\0') {}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Clear() { rep_.assign(WriteBatchInternal::kHeaderSize, '\0'); }

uint32_t WriteBatch::Count() const { return WriteBatchInternal::Count(*this); }

void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::SetCount(this, Count() + source.Count());
  rep_.append(source.rep_, WriteBatchInternal::kHeaderSize);
}

Status WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  b->rep_.assign(contents);
  return Status::OK();
}

namespace {

struct NoopHandler {
  void Put(std::string_view /*key*/, std::string_view /*value*/) {}
  void Delete(std::string_view /*key*/) {}
};

struct MemTableInserter {
  SequenceNumber sequence;
  MemTable* mem;

  void Put(std::string_view key, std::string_view value) {
    mem->Add(sequence++, ValueType::kValue, key, value);
  }
  void Delete(std::string_view key) { mem->Add(sequence++, ValueType::kDeletion, key, {}); }
};

}  // namespace

Status WriteBatchInternal::Validate(std::string_view contents) {
  return Iterate(contents, NoopHandler{});
}

Status WriteBatchInternal::InsertInto(std::string_view contents, MemTable* mem) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  return Iterate(contents, MemTableInserter{DecodeFixed64(contents.data()), mem});
}

}  // namespace talus
