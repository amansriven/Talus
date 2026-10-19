#include "db/db_iter.h"

#include <algorithm>
#include <string>
#include <utility>

#include "db/super_version.h"

namespace talus {
namespace {

class MemTableIterator final : public InternalIterator {
 public:
  explicit MemTableIterator(const MemTable* mem) : iter_(mem->NewIterator()) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(std::string_view target) override { iter_.Seek(target); }
  void Next() override { iter_.Next(); }
  std::string_view key() const override { return iter_.key(); }
  std::string_view value() const override { return iter_.value(); }
  Status status() const override { return Status::OK(); }  // In-memory: cannot fail.

 private:
  MemTable::Iterator iter_;
};

class LevelIterator final : public InternalIterator {
 public:
  LevelIterator(const std::vector<FilePtr>* files, TableCache* table_cache,
                const TableReadOptions& options)
      : files_(files), table_cache_(table_cache), options_(options) {}

  bool Valid() const override { return file_iter_ != nullptr && file_iter_->Valid(); }

  void SeekToFirst() override {
    OpenFile(0);
    if (file_iter_ != nullptr) {
      file_iter_->SeekToFirst();
    }
    SkipExhaustedFiles();
  }

  void Seek(std::string_view target) override {
    // The first file whose largest key is >= target.
    const InternalKeyComparator cmp;
    const auto it = std::lower_bound(
        files_->begin(), files_->end(), target,
        [&](const FilePtr& f, std::string_view t) { return cmp(f->largest, t) < 0; });
    OpenFile(static_cast<size_t>(it - files_->begin()));
    if (file_iter_ != nullptr) {
      file_iter_->Seek(target);
    }
    SkipExhaustedFiles();
  }

  void Next() override {
    file_iter_->Next();
    SkipExhaustedFiles();
  }

  std::string_view key() const override { return file_iter_->key(); }
  std::string_view value() const override { return file_iter_->value(); }
  Status status() const override {
    return file_iter_ != nullptr ? file_iter_->status() : Status::OK();
  }

 private:
  void OpenFile(size_t index) {
    index_ = index;
    file_iter_.reset();
    if (index_ < files_->size()) {
      const FileMetaData& f = *(*files_)[index_];
      file_iter_ = table_cache_->NewIterator(f.number, f.file_size, options_);
    }
  }

  // Advances to the next file while the current one is exhausted. Stops (and
  // keeps the failed iterator, so status() reports it) on any error.
  void SkipExhaustedFiles() {
    while (file_iter_ != nullptr && !file_iter_->Valid()) {
      if (!file_iter_->status().ok()) {
        return;
      }
      OpenFile(index_ + 1);
      if (file_iter_ != nullptr) {
        file_iter_->SeekToFirst();
      }
    }
  }

  const std::vector<FilePtr>* const files_;
  TableCache* const table_cache_;
  const TableReadOptions options_;
  size_t index_ = 0;
  std::unique_ptr<InternalIterator> file_iter_;
};

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<InternalIterator> iter, SequenceNumber sequence,
         SuperVersion* super_version)
      : iter_(std::move(iter)), sequence_(sequence), super_version_(super_version) {}

  ~DBIter() override {
    iter_.reset();  // Memtable and table iterators go before what they read.
    super_version_->Unref();
  }

  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    status_ = Status::OK();
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    status_ = Status::OK();
    // Sorts before every entry for `target` visible at our sequence.
    seek_key_.clear();
    AppendInternalKey(&seek_key_, target, sequence_, kValueTypeForSeek);
    iter_->Seek(seek_key_);
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    // Skip the rest of the current user key's (older) versions.
    skip_key_.assign(ExtractUserKey(iter_->key()));
    iter_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

  std::string_view key() const override { return ExtractUserKey(iter_->key()); }
  std::string_view value() const override { return iter_->value(); }

  Status status() const override { return status_.ok() ? iter_->status() : status_; }

 private:
  // Moves to the next entry that is a key's newest visible version and is
  // not a deletion. With `skipping`, entries for user keys <= skip_key_ are
  // older versions of keys already handled.
  void FindNextUserEntry(bool skipping) {
    for (; iter_->Valid(); iter_->Next()) {
      ParsedInternalKey ikey;
      if (!ParseInternalKey(iter_->key(), &ikey)) {
        status_ = Status::Corruption("corrupted internal key in DB iterator");
        valid_ = false;
        return;
      }
      if (ikey.sequence > sequence_) {
        continue;  // Written after our read point.
      }
      if (skipping && ikey.user_key <= std::string_view(skip_key_)) {
        continue;  // An older version of a key already yielded or deleted.
      }
      if (ikey.type == ValueType::kDeletion) {
        // The newest visible version is a deletion: hide the whole key.
        skip_key_.assign(ikey.user_key);
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
    valid_ = false;  // Exhausted, or the merged iterator hit an error.
  }

  std::unique_ptr<InternalIterator> iter_;
  const SequenceNumber sequence_;
  SuperVersion* const super_version_;
  bool valid_ = false;
  Status status_;
  std::string skip_key_;
  std::string seek_key_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMemTableIterator(const MemTable* mem) {
  return std::make_unique<MemTableIterator>(mem);
}

std::unique_ptr<InternalIterator> NewLevelIterator(const std::vector<FilePtr>* files,
                                                   TableCache* table_cache,
                                                   const TableReadOptions& options) {
  return std::make_unique<LevelIterator>(files, table_cache, options);
}

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<InternalIterator> internal,
                                        SequenceNumber sequence, SuperVersion* super_version) {
  return std::make_unique<DBIter>(std::move(internal), sequence, super_version);
}

}  // namespace talus
