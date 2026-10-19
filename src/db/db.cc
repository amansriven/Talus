#include "Talus/db.h"

#include <utility>

#include "db/db_impl.h"

namespace talus {

DB::DB(PrivateTag /*tag*/, std::unique_ptr<DBImpl> impl) : impl_(std::move(impl)) {}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  std::unique_ptr<DBImpl> impl;
  Status s = DBImpl::Open(options, path, &impl);
  if (s.ok()) {
    *db = std::make_unique<DB>(PrivateTag{}, std::move(impl));
  }
  return s;
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  return impl_->Put(options, key, value);
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  return impl_->Delete(options, key);
}

Status DB::Write(const WriteOptions& options, WriteBatch* batch) {
  return impl_->Write(options, batch);
}

Status DB::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  return impl_->Get(options, key, value);
}

Status DB::Get(const ReadOptions& options, std::string_view key, PinnableValue* value) {
  return impl_->Get(options, key, value);
}

std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& options) {
  return impl_->NewIterator(options);
}

const Snapshot* DB::GetSnapshot() { return impl_->GetSnapshot(); }

void DB::ReleaseSnapshot(const Snapshot* snapshot) { impl_->ReleaseSnapshot(snapshot); }

Status DB::Flush() { return impl_->Flush(); }

Status DB::CompactRange(std::string_view begin, std::string_view end) {
  return impl_->CompactRange(begin, end);
}

DBStats DB::GetStats() const { return impl_->GetStats(); }

Status DB::WaitForBackgroundWork() { return impl_->WaitForBackgroundWork(); }

}  // namespace talus
