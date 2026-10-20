#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Talus/status.h"
#include "cache/table_cache.h"
#include "db/dbformat.h"
#include "version/version_edit.h"

namespace talus {

namespace log {
class Writer;
}

// The SSTable numbers referenced by some live Version. A file is obsolete
// (safe to delete) exactly when it is absent from here and not a pending
// compaction or flush output.
class LiveFiles {
 public:
  void Add(uint64_t number);
  void Remove(uint64_t number);
  void CopyTo(std::set<uint64_t>* out) const;

 private:
  mutable std::mutex mu_;
  std::multiset<uint64_t> numbers_;
};

using FilePtr = std::shared_ptr<const FileMetaData>;

// An immutable snapshot of the SSTables at each level. L0 is sorted by file
// number (oldest first) and its files may overlap. L1 and below are sorted
// by key and non-overlapping, and every user key's versions sit in a single
// file per level (compaction only cuts outputs between user keys).
// Reference-counted through shared_ptr; files stay live while any Version
// holding them does.
class Version {
 public:
  explicit Version(int num_levels) : files_(num_levels) {}

  int num_levels() const { return static_cast<int>(files_.size()); }
  const std::vector<FilePtr>& files(int level) const { return files_[level]; }
  uint64_t LevelBytes(int level) const;
  // Per-level compaction scores (L0 to the second-to-last level).
  const std::vector<double>& compaction_scores() const { return scores_; }

  // Point lookup, newest data first: L0 files containing the key by
  // descending file number, then one file per deeper level found by binary
  // search. For the newest entry of the user key visible at the lookup
  // sequence, calls fn(value, pin) and returns OK if it is a value, or
  // returns NotFound if it is a deletion. Returns NotFound if there is no
  // entry. Adds the number of tables probed to *tables_consulted. Does not
  // allocate on block-cache hits.
  template <typename Fn>
  Status Get(const LookupKey& key, TableCache* table_cache, const TableReadOptions& options,
             Fn&& fn, int* tables_consulted) const;

  // Appends to *inputs the files in `level` whose user-key range overlaps
  // [*begin, *end] (null = unbounded). For L0 the range grows to cover every
  // overlapping file, transitively, since L0 files may overlap each other.
  void GetOverlappingInputs(int level, const std::string_view* begin, const std::string_view* end,
                            std::vector<FilePtr>* inputs) const;

 private:
  friend class VersionSet;

  std::vector<std::vector<FilePtr>> files_;
  std::vector<double> scores_;
};

// A compaction of inputs(0) at level() with the overlapping inputs(1) at
// level() + 1, producing files at level() + 1.
class Compaction {
 public:
  int level() const { return level_; }
  int output_level() const { return level_ + 1; }
  const std::vector<FilePtr>& inputs(int which) const { return inputs_[which]; }
  const Version& input_version() const { return *input_version_; }
  VersionEdit* edit() { return &edit_; }
  uint64_t max_output_file_size() const { return max_output_file_size_; }
  uint64_t InputBytes() const;

  // Adds the deletion of every input file to `edit`.
  void AddInputDeletions(VersionEdit* edit) const;

  // True if no level below the output level can hold `user_key`, so a
  // tombstone for it may be dropped. Calls must use ascending user keys.
  bool IsBaseLevelForKey(std::string_view user_key);

  // True if the current output should end before `internal_key` because it
  // already overlaps too many bytes of level() + 2. That would make a future
  // compaction of it expensive. Calls must use ascending keys.
  bool ShouldStopBefore(std::string_view internal_key);

 private:
  friend class VersionSet;

  int level_ = 0;
  uint64_t max_output_file_size_ = 0;
  uint64_t max_grandparent_overlap_ = 0;
  std::shared_ptr<const Version> input_version_;
  std::vector<FilePtr> inputs_[2];
  std::vector<FilePtr> grandparents_;
  VersionEdit edit_;

  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  uint64_t overlapped_bytes_ = 0;
  std::vector<size_t> level_ptrs_;
};

// The current Version, the MANIFEST, and file numbering. All methods
// require the DB mutex unless stated otherwise.
class VersionSet {
 public:
  struct Options {
    int num_levels = 7;
    int l0_compaction_trigger = 4;
    uint64_t l1_max_bytes = 10 << 20;
    int level_size_multiplier = 10;
    uint64_t target_file_size = 2 << 20;
  };

  VersionSet(std::string dbname, const Options& options);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Initializes a new database: writes MANIFEST-000001 and CURRENT.
  Status CreateNew();

  // Loads the state named by CURRENT. A torn final MANIFEST record (a crash
  // mid-write) is ignored. Any other damage is Corruption.
  Status Recover();

  // Replaces the current Version with an identical one whose unpinned files
  // carry the tables `pin` returns (null leaves a file unpinned). For Open
  // only, before any reader or background thread can hold the Version, so
  // tables found at recovery get the same lock-free reads as new ones.
  void AttachTables(
      const std::function<std::shared_ptr<const PinnedTable>(const FileMetaData&)>& pin);

  // Durably applies `edit`, then makes the result current. It fills in the
  // edit's file-number and sequence counters, appends it to the MANIFEST and
  // syncs. The first call after Recover()/CreateNew() starts a fresh
  // MANIFEST holding a full snapshot, then atomically switches CURRENT to
  // it. Releases *db_lock during I/O; concurrent calls are serialized. On
  // failure the current version is unchanged, and the next call starts a
  // fresh MANIFEST rather than appending after a possibly partial record.
  Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* db_lock);

  std::shared_ptr<const Version> current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) {
      next_file_number_ = number + 1;
    }
  }
  // WAL files with numbers below this are no longer needed.
  uint64_t log_number() const { return log_number_; }
  uint64_t manifest_file_number() const { return manifest_file_number_; }
  SequenceNumber last_sequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }

  // Adds every SSTable number referenced by a live Version. Thread-safe.
  void AddLiveFiles(std::set<uint64_t>* live) const { live_files_.CopyTo(live); }

  // The highest-scoring compaction with score >= 1 whose two levels are not
  // busy, or nullptr. Advances that level's round-robin compaction pointer.
  std::unique_ptr<Compaction> PickCompaction(const std::vector<bool>& busy_levels);

  // A compaction of the files in `level` overlapping [*begin, *end], or
  // nullptr if there are none. For level > 0 the inputs are capped at about
  // 25 target-size files, so callers loop until this returns nullptr.
  std::unique_ptr<Compaction> CompactRange(int level, const std::string_view* begin,
                                           const std::string_view* end);

  double MaxBytesForLevel(int level) const;
  const Options& options() const { return options_; }

 private:
  // Builds base + edit, checking that L1+ files are sorted, non-overlapping
  // and never share a user key. Then computes compaction scores.
  Status Apply(const Version& base, const VersionEdit& edit, std::shared_ptr<Version>* result);
  FilePtr MakeFile(const FileMetaData& meta);
  std::unique_ptr<Compaction> SetupCompaction(int level, std::vector<FilePtr> inputs);

  const std::string dbname_;
  const Options options_;
  // Declared before current_, so it is destroyed after the versions that
  // reference it.
  LiveFiles live_files_;
  std::shared_ptr<const Version> current_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  std::vector<std::string> compact_pointers_;  // Per level; empty = start of level.

  std::unique_ptr<log::Writer> manifest_;  // Null until the first LogAndApply.
  bool manifest_busy_ = false;
  std::condition_variable manifest_cv_;
};

template <typename Fn>
Status Version::Get(const LookupKey& key, TableCache* table_cache, const TableReadOptions& options,
                    Fn&& fn, int* tables_consulted) const {
  const std::string_view internal_key = key.internal_key();
  const std::string_view user_key = key.user_key();
  enum class State { kNotFound, kFound, kDeleted, kCorrupt };

  auto probe = [&](const FileMetaData& f, State* state) {
    ++*tables_consulted;
    auto on_entry = [&](std::string_view found, std::string_view value, Pin& pin) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(found, &parsed)) {
        *state = State::kCorrupt;
      } else if (parsed.user_key == user_key) {
        if (parsed.type == ValueType::kValue) {
          *state = State::kFound;
          fn(value, pin);
        } else {
          *state = State::kDeleted;
        }
      }
    };
    // A pinned table skips the table cache's lookup and shard mutex.
    if (f.table != nullptr) {
      return TableCache::Get(*f.table, internal_key, options, on_entry);
    }
    return table_cache->Get(f.number, f.file_size, internal_key, options, on_entry);
  };
  auto finish = [](State state) {
    switch (state) {
      case State::kFound:
        return Status::OK();
      case State::kDeleted:
        return Status::NotFound();
      case State::kCorrupt:
        return Status::Corruption("corrupted internal key in sstable");
      case State::kNotFound:
        break;
    }
    return Status::NotFound();
  };

  // L0: newest file first. Iterating in place (no candidate list) keeps the
  // lookup allocation-free.
  const auto& l0 = files_[0];
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    const FileMetaData& f = **it;
    if (user_key < ExtractUserKey(f.smallest) || user_key > ExtractUserKey(f.largest)) {
      continue;
    }
    State state = State::kNotFound;
    Status s = probe(f, &state);
    if (!s.ok()) {
      return s;
    }
    if (state != State::kNotFound) {
      return finish(state);
    }
  }
  const InternalKeyComparator cmp;
  for (int level = 1; level < num_levels(); ++level) {
    const auto& files = files_[level];
    // The first file whose largest key is >= the lookup key.
    auto it = std::lower_bound(
        files.begin(), files.end(), internal_key,
        [&](const FilePtr& f, std::string_view k) { return cmp(f->largest, k) < 0; });
    if (it == files.end() || user_key < ExtractUserKey((*it)->smallest)) {
      continue;
    }
    State state = State::kNotFound;
    Status s = probe(**it, &state);
    if (!s.ok()) {
      return s;
    }
    if (state != State::kNotFound) {
      return finish(state);
    }
  }
  return Status::NotFound();
}

}  // namespace talus
