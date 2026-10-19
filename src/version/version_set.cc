#include "version/version_set.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "util/file.h"
#include "util/filename.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace talus {
namespace {

// FileMetaData that registers its number as live for its lifetime. Versions
// share these through shared_ptr, so the number leaves the registry exactly
// when the last Version holding the file is destroyed.
struct LiveFileMetaData : FileMetaData {
  LiveFileMetaData(const FileMetaData& meta, LiveFiles* registry)
      : FileMetaData(meta), registry(registry) {
    registry->Add(number);
  }
  ~LiveFileMetaData() { registry->Remove(number); }
  LiveFileMetaData(const LiveFileMetaData&) = delete;
  LiveFileMetaData& operator=(const LiveFileMetaData&) = delete;

  LiveFiles* const registry;
};

std::string_view UserKey(const std::string& internal_key) { return ExtractUserKey(internal_key); }

// The smallest and largest internal keys across `files`.
void GetRange(const std::vector<FilePtr>& a, const std::vector<FilePtr>& b, std::string* smallest,
              std::string* largest) {
  const InternalKeyComparator cmp;
  bool first = true;
  for (const auto* files : {&a, &b}) {
    for (const FilePtr& f : *files) {
      if (first || cmp(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (first || cmp(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
      first = false;
    }
  }
}

}  // namespace

void LiveFiles::Add(uint64_t number) {
  std::lock_guard<std::mutex> lock(mu_);
  numbers_.insert(number);
}

void LiveFiles::Remove(uint64_t number) {
  std::lock_guard<std::mutex> lock(mu_);
  numbers_.erase(numbers_.find(number));
}

void LiveFiles::CopyTo(std::set<uint64_t>* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out->insert(numbers_.begin(), numbers_.end());
}

uint64_t Version::LevelBytes(int level) const {
  uint64_t total = 0;
  for (const FilePtr& f : files_[level]) {
    total += f->file_size;
  }
  return total;
}

void Version::GetOverlappingInputs(int level, const std::string_view* begin,
                                   const std::string_view* end,
                                   std::vector<FilePtr>* inputs) const {
  // Owned copies, because an L0 search may widen the range.
  std::string user_begin = begin != nullptr ? std::string(*begin) : std::string();
  std::string user_end = end != nullptr ? std::string(*end) : std::string();
  const auto& files = files_[level];
  inputs->clear();
  for (size_t i = 0; i < files.size();) {
    const FilePtr& f = files[i++];
    const std::string_view file_start = UserKey(f->smallest);
    const std::string_view file_limit = UserKey(f->largest);
    if (begin != nullptr && file_limit < std::string_view(user_begin)) {
      continue;  // Entirely before the range.
    }
    if (end != nullptr && file_start > std::string_view(user_end)) {
      continue;  // Entirely after the range.
    }
    inputs->push_back(f);
    if (level == 0) {
      // An L0 file may stick out of the range; widen it and restart, so every
      // L0 file overlapping the widened range is included too.
      if (begin != nullptr && file_start < std::string_view(user_begin)) {
        user_begin.assign(file_start);
        inputs->clear();
        i = 0;
      } else if (end != nullptr && file_limit > std::string_view(user_end)) {
        user_end.assign(file_limit);
        inputs->clear();
        i = 0;
      }
    }
  }
}

uint64_t Compaction::InputBytes() const {
  uint64_t total = 0;
  for (const auto& files : inputs_) {
    for (const FilePtr& f : files) {
      total += f->file_size;
    }
  }
  return total;
}

void Compaction::AddInputDeletions(VersionEdit* edit) const {
  for (int which = 0; which < 2; ++which) {
    for (const FilePtr& f : inputs_[which]) {
      edit->DeleteFile(level_ + which, f->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(std::string_view user_key) {
  // Levels below the output level. Each keeps a cursor, since keys arrive in
  // ascending order.
  for (int level = level_ + 2; level < input_version_->num_levels(); ++level) {
    const auto& files = input_version_->files(level);
    while (level_ptrs_[level] < files.size()) {
      const FilePtr& f = files[level_ptrs_[level]];
      if (user_key <= UserKey(f->largest)) {
        if (user_key >= UserKey(f->smallest)) {
          return false;  // This file's range holds the key.
        }
        break;
      }
      ++level_ptrs_[level];
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(std::string_view internal_key) {
  const InternalKeyComparator cmp;
  while (grandparent_index_ < grandparents_.size() &&
         cmp(internal_key, grandparents_[grandparent_index_]->largest) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    ++grandparent_index_;
  }
  seen_key_ = true;
  if (overlapped_bytes_ > max_grandparent_overlap_) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

VersionSet::VersionSet(std::string dbname, const Options& options)
    : dbname_(std::move(dbname)),
      options_(options),
      current_(std::make_shared<Version>(options.num_levels)),
      compact_pointers_(options.num_levels) {}

VersionSet::~VersionSet() = default;

double VersionSet::MaxBytesForLevel(int level) const {
  return static_cast<double>(options_.l1_max_bytes) *
         std::pow(static_cast<double>(options_.level_size_multiplier), level - 1);
}

FilePtr VersionSet::MakeFile(const FileMetaData& meta) {
  return std::make_shared<LiveFileMetaData>(meta, &live_files_);
}

Status VersionSet::Apply(const Version& base, const VersionEdit& edit,
                         std::shared_ptr<Version>* result) {
  auto v = std::make_shared<Version>(options_.num_levels);
  for (int level = 0; level < options_.num_levels; ++level) {
    for (const FilePtr& f : base.files_[level]) {
      if (edit.deleted_files.count({level, f->number}) == 0) {
        v->files_[level].push_back(f);
      }
    }
  }
  for (const auto& [level, meta] : edit.new_files) {
    if (level >= options_.num_levels) {
      return Status::Corruption("VersionEdit adds a file beyond num_levels");
    }
    v->files_[level].push_back(MakeFile(meta));
  }

  const InternalKeyComparator cmp;
  auto& l0 = v->files_[0];
  std::sort(l0.begin(), l0.end(),
            [](const FilePtr& a, const FilePtr& b) { return a->number < b->number; });
  for (int level = 1; level < options_.num_levels; ++level) {
    auto& files = v->files_[level];
    std::sort(files.begin(), files.end(), [&](const FilePtr& a, const FilePtr& b) {
      return cmp(a->smallest, b->smallest) < 0;
    });
    for (size_t i = 1; i < files.size(); ++i) {
      // Strictly increasing user keys: no overlap, and no user key split
      // across two files.
      if (UserKey(files[i - 1]->largest) >= UserKey(files[i]->smallest)) {
        return Status::Corruption("overlapping files in level " + std::to_string(level));
      }
    }
  }

  // Compaction scores: L0 by file count (each L0 file may need probing on
  // every read), deeper levels by bytes against their budget.
  v->scores_.assign(options_.num_levels - 1, 0.0);
  for (int level = 0; level < options_.num_levels - 1; ++level) {
    v->scores_[level] =
        level == 0 ? static_cast<double>(v->files_[0].size()) / options_.l0_compaction_trigger
                   : static_cast<double>(v->LevelBytes(level)) / MaxBytesForLevel(level);
  }
  *result = std::move(v);
  return Status::OK();
}

Status VersionSet::CreateNew() {
  VersionEdit edit;
  edit.SetLogNumber(0);
  edit.SetNextFileNumber(2);
  edit.SetLastSequence(0);
  std::string record;
  edit.EncodeTo(&record);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Create(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer writer(std::move(file));
    s = writer.AddRecord(record);
    if (s.ok()) {
      s = writer.Sync();
    }
    if (s.ok()) {
      s = writer.file()->Close();
    }
  }
  if (s.ok()) {
    s = SetCurrentFile(dbname_, 1);
  }
  if (!s.ok()) {
    (void)RemoveFile(manifest);
  }
  return s;
}

Status VersionSet::Recover() {
  // CURRENT holds "MANIFEST-NNNNNN\n".
  std::unique_ptr<SequentialFile> current_file;
  Status s = SequentialFile::Open(CurrentFileName(dbname_), &current_file);
  if (!s.ok()) {
    return s;
  }
  std::string current(static_cast<size_t>(current_file->size()), '\0');
  size_t n = 0;
  s = current_file->Read(current.size(), current.data(), &n);
  if (!s.ok()) {
    return s;
  }
  uint64_t manifest_number = 0;
  FileType type{};
  if (n != current.size() || current.empty() || current.back() != '\n' ||
      !ParseFileName(std::string_view(current).substr(0, current.size() - 1), &manifest_number,
                     &type) ||
      type != FileType::kDescriptorFile) {
    return Status::Corruption("CURRENT does not name a MANIFEST");
  }

  const std::string manifest = DescriptorFileName(dbname_, manifest_number);
  if (!FileExists(manifest)) {
    return Status::Corruption(manifest, "named by CURRENT but missing");
  }
  std::unique_ptr<SequentialFile> manifest_file;
  s = SequentialFile::Open(manifest, &manifest_file);
  if (!s.ok()) {
    return s;
  }
  log::Reader reader(std::move(manifest_file));
  std::map<uint64_t, std::pair<int, FileMetaData>> files;  // number -> (level, meta)
  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file;
  std::optional<SequenceNumber> last_sequence;
  std::vector<std::string> compact_pointers(options_.num_levels);
  std::string scratch;
  std::string_view record;
  bool done = false;
  while (!done) {
    switch (reader.ReadRecord(&record, &scratch)) {
      case log::Reader::Result::kEof:
        done = true;  // Includes a torn final record from a crash mid-write.
        break;
      case log::Reader::Result::kCorruption:
        return Status::Corruption(manifest, reader.error().message());
      case log::Reader::Result::kIOError:
        return reader.error();
      case log::Reader::Result::kRecord: {
        VersionEdit edit;
        s = edit.DecodeFrom(record);
        if (!s.ok()) {
          return Status::Corruption(manifest, s.message());
        }
        for (const auto& [level, number] : edit.deleted_files) {
          files.erase(number);
        }
        for (const auto& [level, meta] : edit.new_files) {
          files[meta.number] = {level, meta};
        }
        for (const auto& [level, key] : edit.compact_pointers) {
          if (level < options_.num_levels) {
            compact_pointers[level] = key;
          }
        }
        if (edit.log_number) {
          log_number = edit.log_number;
        }
        if (edit.next_file_number) {
          next_file = edit.next_file_number;
        }
        if (edit.last_sequence) {
          last_sequence = edit.last_sequence;
        }
        break;
      }
    }
  }
  if (!log_number || !next_file || !last_sequence) {
    return Status::Corruption(manifest, "missing log number, next file number or last sequence");
  }

  VersionEdit all;
  for (const auto& [number, entry] : files) {
    all.AddFile(entry.first, entry.second);
  }
  std::shared_ptr<Version> v;
  s = Apply(Version(options_.num_levels), all, &v);
  if (!s.ok()) {
    return Status::Corruption(manifest, s.message());
  }
  current_ = std::move(v);
  compact_pointers_ = std::move(compact_pointers);
  next_file_number_ = *next_file;
  log_number_ = *log_number;
  last_sequence_ = *last_sequence;
  manifest_file_number_ = manifest_number;
  MarkFileNumberUsed(manifest_number);
  MarkFileNumberUsed(log_number_);
  for (const auto& [number, entry] : files) {
    MarkFileNumberUsed(number);
  }
  manifest_.reset();  // The first LogAndApply starts a fresh MANIFEST.
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* db_lock) {
  manifest_cv_.wait(*db_lock, [this] { return !manifest_busy_; });
  manifest_busy_ = true;

  // A fresh MANIFEST needs its number before the edit records next_file.
  const bool new_manifest = manifest_ == nullptr;
  const uint64_t new_manifest_number = new_manifest ? NewFileNumber() : manifest_file_number_;
  if (!edit->log_number) {
    edit->SetLogNumber(log_number_);
  }
  edit->SetNextFileNumber(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  std::shared_ptr<Version> v;
  Status s = Apply(*current_, *edit, &v);
  std::string record;
  edit->EncodeTo(&record);
  std::string snapshot;
  if (s.ok() && new_manifest) {
    // A full snapshot of the current state, followed by `edit`.
    VersionEdit full;
    for (int level = 0; level < options_.num_levels; ++level) {
      if (!compact_pointers_[level].empty()) {
        full.SetCompactPointer(level, compact_pointers_[level]);
      }
      for (const FilePtr& f : current_->files(level)) {
        full.AddFile(level, *f);
      }
    }
    full.EncodeTo(&snapshot);
  }

  if (s.ok()) {
    db_lock->unlock();
    if (new_manifest) {
      std::unique_ptr<WritableFile> file;
      s = WritableFile::Create(DescriptorFileName(dbname_, new_manifest_number), &file);
      if (s.ok()) {
        manifest_ = std::make_unique<log::Writer>(std::move(file));
        s = manifest_->AddRecord(snapshot);
      }
    }
    if (s.ok()) {
      s = manifest_->AddRecord(record);
    }
    if (s.ok()) {
      s = manifest_->Sync();
    }
    if (s.ok() && new_manifest) {
      s = SetCurrentFile(dbname_, new_manifest_number);
    }
    db_lock->lock();
  }

  if (s.ok()) {
    current_ = std::move(v);
    log_number_ = *edit->log_number;
    for (const auto& [level, key] : edit->compact_pointers) {
      compact_pointers_[level] = key;
    }
    if (new_manifest) {
      manifest_file_number_ = new_manifest_number;
    }
  } else if (manifest_ != nullptr) {
    // The MANIFEST may now end in a partial record; never append after it.
    manifest_.reset();
    if (new_manifest) {
      (void)RemoveFile(DescriptorFileName(dbname_, new_manifest_number));
    }
  }
  manifest_busy_ = false;
  manifest_cv_.notify_all();
  return s;
}

std::unique_ptr<Compaction> VersionSet::SetupCompaction(int level, std::vector<FilePtr> inputs) {
  auto c = std::make_unique<Compaction>();
  c->level_ = level;
  c->max_output_file_size_ = options_.target_file_size;
  c->max_grandparent_overlap_ = 10 * options_.target_file_size;
  c->input_version_ = current_;
  c->inputs_[0] = std::move(inputs);
  c->level_ptrs_.assign(options_.num_levels, 0);

  std::string smallest;
  std::string largest;
  GetRange(c->inputs_[0], {}, &smallest, &largest);
  std::string_view begin = UserKey(smallest);
  std::string_view end = UserKey(largest);
  current_->GetOverlappingInputs(level + 1, &begin, &end, &c->inputs_[1]);

  if (level + 2 < options_.num_levels) {
    std::string all_smallest;
    std::string all_largest;
    GetRange(c->inputs_[0], c->inputs_[1], &all_smallest, &all_largest);
    std::string_view all_begin = UserKey(all_smallest);
    std::string_view all_end = UserKey(all_largest);
    current_->GetOverlappingInputs(level + 2, &all_begin, &all_end, &c->grandparents_);
  }

  // Round-robin: the next compaction of this level starts after this one.
  compact_pointers_[level] = largest;
  c->edit_.SetCompactPointer(level, largest);
  return c;
}

std::unique_ptr<Compaction> VersionSet::PickCompaction(const std::vector<bool>& busy_levels) {
  const auto& scores = current_->compaction_scores();
  int level = -1;
  for (int l = 0; l < options_.num_levels - 1; ++l) {
    if (scores[l] >= 1 && !busy_levels[l] && !busy_levels[l + 1] &&
        (level < 0 || scores[l] > scores[level])) {
      level = l;
    }
  }
  if (level < 0) {
    return nullptr;
  }

  // Start at the first file past the compaction pointer, wrapping around.
  const InternalKeyComparator cmp;
  const auto& files = current_->files(level);
  FilePtr start = files.front();
  for (const FilePtr& f : files) {
    if (compact_pointers_[level].empty() || cmp(f->largest, compact_pointers_[level]) > 0) {
      start = f;
      break;
    }
  }
  std::vector<FilePtr> inputs{start};
  if (level == 0) {
    // Overlapping L0 files must move down together, or an older version of
    // a key could end up above a newer one.
    std::string_view begin = UserKey(start->smallest);
    std::string_view end = UserKey(start->largest);
    current_->GetOverlappingInputs(0, &begin, &end, &inputs);
  }
  return SetupCompaction(level, std::move(inputs));
}

std::unique_ptr<Compaction> VersionSet::CompactRange(int level, const std::string_view* begin,
                                                     const std::string_view* end) {
  std::vector<FilePtr> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) {
    return nullptr;
  }
  if (level > 0) {
    // Bound the work per call; the caller loops until the range is done.
    const uint64_t limit = 25 * options_.target_file_size;
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
      total += inputs[i]->file_size;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }
  return SetupCompaction(level, std::move(inputs));
}

}  // namespace talus
