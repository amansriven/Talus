#include "db/db_impl.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>

#include "db/db_iter.h"
#include "db/log_replay.h"
#include "db/write_batch_internal.h"
#include "table/merging_iterator.h"
#include "util/filename.h"
#include "util/port.h"
#include "wal/log_format.h"

namespace talus {
namespace {

uint64_t NowMicros() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

Status ValidateOptions(const Options& o) {
  if (o.num_levels < 2 || o.num_levels > VersionEdit::kMaxLevels) {
    return Status::InvalidArgument("num_levels must be in [2, 64]");
  }
  if (o.l0_compaction_trigger < 1 || o.l0_slowdown_trigger < o.l0_compaction_trigger ||
      o.l0_stop_trigger < o.l0_slowdown_trigger) {
    return Status::InvalidArgument(
        "need 1 <= l0_compaction_trigger <= l0_slowdown_trigger <= l0_stop_trigger");
  }
  if (o.memtable_size == 0 || o.block_size == 0 || o.block_restart_interval < 1 ||
      o.target_file_size == 0 || o.l1_max_bytes == 0 || o.level_size_multiplier < 1 ||
      o.max_background_compactions < 1 || o.max_open_files < 1 || o.block_cache_shards < 1 ||
      o.bloom_bits_per_key < 0) {
    return Status::InvalidArgument("option out of range");
  }
  if (!CompressionSupported(o.compression)) {
    return Status::InvalidArgument("compression type not supported by this build");
  }
  return Status::OK();
}

TableOptions MakeTableOptions(const Options& o) {
  TableOptions t;
  t.block_size = o.block_size;
  t.block_restart_interval = o.block_restart_interval;
  t.bloom_bits_per_key = o.bloom_bits_per_key;
  t.filter_type = o.bloom_bits_per_key == 0    ? FilterType::kNone
                  : o.use_blocked_bloom_filter ? FilterType::kBlockedBloom
                                               : FilterType::kStandardBloom;
  t.compression = o.compression;
  return t;
}

}  // namespace

// ---- Open, recovery, shutdown ----------------------------------------------

DBImpl::DBImpl(PrivateTag /*tag*/, const Options& options, std::string dbname)
    : options_(options),
      dbname_(std::move(dbname)),
      table_options_(MakeTableOptions(options)),
      buffer_pool_(options.use_huge_pages ? std::make_unique<HugePageBufferPool>() : nullptr),
      block_cache_(
          options.block_cache_capacity > 0
              ? std::make_unique<BlockCache>(options.block_cache_capacity,
                                             static_cast<size_t>(options.block_cache_shards))
              : nullptr),
      table_cache_(std::make_unique<TableCache>(
          dbname_,
          TableCache::Options{.read_mode = options.use_mmap_reads ? RandomAccessFile::Mode::kMmap
                                                                  : RandomAccessFile::Mode::kPread,
                              .block_cache = block_cache_.get(),
                              .buffer_pool = buffer_pool_.get(),
                              .max_open_files = static_cast<size_t>(options.max_open_files)})),
      rate_limiter_(std::make_unique<RateLimiter>(options.compaction_rate_limit_bytes_per_sec)),
      versions_(std::make_unique<VersionSet>(
          dbname_, VersionSet::Options{.num_levels = options.num_levels,
                                       .l0_compaction_trigger = options.l0_compaction_trigger,
                                       .l1_max_bytes = options.l1_max_bytes,
                                       .level_size_multiplier = options.level_size_multiplier,
                                       .target_file_size = options.target_file_size})),
      busy_levels_(static_cast<size_t>(options.num_levels), false) {}

Status DBImpl::Open(const Options& options, const std::string& dbname,
                    std::unique_ptr<DBImpl>* db) {
  Status s = ValidateOptions(options);
  if (!s.ok()) {
    return s;
  }
  auto impl = std::make_unique<DBImpl>(PrivateTag{}, options, dbname);
  s = impl->Recover();
  if (!s.ok()) {
    return s;
  }
  impl->StartBackgroundThreads();
  *db = std::move(impl);
  return Status::OK();
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Relaxed: the mutex and the notify below order it; the compaction loop
    // only polls it to stop early.
    shutting_down_.store(true, std::memory_order_relaxed);
  }
  bg_cv_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
  for (std::thread& t : compaction_threads_) {
    t.join();
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (super_version_ != nullptr) {
    super_version_->Unref();
    super_version_ = nullptr;
  }
}

std::shared_ptr<MemTable> DBImpl::NewMemTable() const {
  return std::make_shared<MemTable>(options_.allow_concurrent_memtable_write,
                                    options_.use_huge_pages);
}

Status DBImpl::CreateLog(uint64_t number, std::unique_ptr<log::Writer>* log) {
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Create(LogFileName(dbname_, number), &file);
  if (s.ok()) {
    // The new WAL's directory entry must be durable before a sync write to
    // it is acknowledged.
    s = SyncDirectory(dbname_);
  }
  if (s.ok()) {
    *log = std::make_unique<log::Writer>(std::move(file));
  }
  return s;
}

Status DBImpl::Recover() {
  const bool exists = FileExists(CurrentFileName(dbname_));
  if (!exists && !options_.create_if_missing) {
    return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
  }
  if (exists && options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
  }
  Status s = CreateDirIfMissing(dbname_);
  if (s.ok()) {
    s = FileLock::Acquire(LockFileName(dbname_), &lock_file_);
  }
  if (s.ok() && !exists) {
    s = versions_->CreateNew();
  }
  if (!s.ok()) {
    return s;
  }

  // No other thread exists yet; the lock is held for uniformity with the
  // functions that require it.
  std::unique_lock<std::mutex> lock(mu_);
  s = versions_->Recover();
  if (!s.ok()) {
    return s;
  }

  // Every table the MANIFEST references must exist. Collect the WALs that
  // may hold unflushed writes: those at or above the MANIFEST's log number.
  std::vector<std::string> children;
  s = GetChildren(dbname_, &children);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> tables;
  std::vector<uint64_t> logs;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (ParseFileName(name, &number, &type)) {
      if (type == FileType::kTableFile) {
        tables.insert(number);
      } else if (type == FileType::kLogFile && number >= versions_->log_number()) {
        logs.push_back(number);
      }
    }
  }
  const auto current = versions_->current();
  for (int level = 0; level < current->num_levels(); ++level) {
    for (const FilePtr& f : current->files(level)) {
      if (tables.count(f->number) == 0) {
        return Status::Corruption(TableFileName(dbname_, f->number), "missing sstable");
      }
    }
  }
  // The WAL named by the MANIFEST's log number always exists: it is created
  // (and its directory entry synced) before any edit names it, and deleted
  // only after a later edit names a newer one. If it is missing, a durable
  // MANIFEST edit was lost, e.g. a damaged final record that reads as a
  // crash-torn tail. Then the writes it covered are gone from both the WAL
  // and the MANIFEST, and opening would silently return stale data.
  const uint64_t log_number = versions_->log_number();
  if (log_number != 0 &&
      std::find(logs.begin(), logs.end(), log_number) == logs.end()) {
    return Status::Corruption(LogFileName(dbname_, log_number),
                              "WAL named by the MANIFEST is missing (lost MANIFEST edit?)");
  }

  // Replay WALs oldest first, flushing to L0 whenever the memtable fills,
  // and once more at the end.
  std::sort(logs.begin(), logs.end());
  SequenceNumber max_sequence = versions_->last_sequence();
  VersionEdit edit;
  auto flush_to_l0 = [&](const MemTable& mem) {
    const uint64_t number = versions_->NewFileNumber();
    FileMetaData meta;
    Status bs = BuildTable(mem, number, &meta);
    if (bs.ok() && meta.file_size > 0) {
      edit.AddFile(0, meta);
    }
    return bs;
  };
  auto mem = NewMemTable();
  for (uint64_t number : logs) {
    versions_->MarkFileNumberUsed(number);
    s = ReplayLogFile(LogFileName(dbname_, number), options_.paranoid_checks, mem.get(),
                      &max_sequence);
    if (s.ok() && mem->ApproximateMemoryUsage() > options_.memtable_size) {
      s = flush_to_l0(*mem);
      mem = NewMemTable();
    }
    if (!s.ok()) {
      return s;
    }
  }
  if (!mem->Empty()) {
    s = flush_to_l0(*mem);
  }
  if (s.ok() && !edit.new_files.empty()) {
    s = SyncDirectory(dbname_);  // New tables' entries before the MANIFEST names them.
  }

  // A fresh WAL for new writes. The MANIFEST's log number moves to it, which
  // makes every replayed WAL obsolete.
  logfile_number_ = versions_->NewFileNumber();
  if (s.ok()) {
    s = CreateLog(logfile_number_, &log_);
  }
  if (s.ok()) {
    edit.SetLogNumber(logfile_number_);
    versions_->SetLastSequence(max_sequence);
    s = versions_->LogAndApply(&edit, &lock);
  }
  if (!s.ok()) {
    return s;
  }

  mem_ = NewMemTable();
  GroupCommitter::Hooks hooks;
  hooks.prepare = [this] { return MakeRoomForWrite(); };
  hooks.on_logged = [this](size_t members, size_t bytes) { RecordWriteGroup(members, bytes); };
  committer_ = std::make_unique<GroupCommitter>(log_.get(), mem_.get(), max_sequence,
                                                options_.write_spin_iterations, std::move(hooks));
  InstallSuperVersion();
  DeleteObsoleteFiles(&lock, /*recovering=*/true);
  return Status::OK();
}

void DBImpl::StartBackgroundThreads() {
  flush_thread_ = std::thread([this] { FlushThreadMain(); });
  for (int i = 0; i < options_.max_background_compactions; ++i) {
    compaction_threads_.emplace_back([this] { CompactionThreadMain(); });
  }
}

// ---- Write path ------------------------------------------------------------

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
  if (batch == nullptr) {
    return Status::InvalidArgument("null WriteBatch");
  }
  per_thread_.Local().Add(Counter::kUserBytesWritten,
                          batch->ApproximateSize() - WriteBatchInternal::kHeaderSize);
  return committer_->Write(options, batch);
}

// Put and Delete reuse one batch per thread: Clear() keeps its capacity, so
// in steady state a single-key write does not allocate (spec §9).
Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  thread_local WriteBatch batch;
  batch.Clear();
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  thread_local WriteBatch batch;
  batch.Clear();
  batch.Delete(key);
  return Write(options, &batch);
}

void DBImpl::RecordWriteGroup(size_t members, size_t wal_bytes) {
  ThreadSlot& slot = per_thread_.Local();
  slot.Add(Counter::kWriteGroups);
  slot.Add(Counter::kWalBytesWritten, wal_bytes + log::kHeaderSize);
  const size_t bucket = std::min<size_t>(static_cast<size_t>(std::bit_width(members)) - 1, 6);
  slot.Add(static_cast<Counter>(static_cast<size_t>(Counter::kGroupSize1) + bucket));
}

Status DBImpl::MakeRoomForWrite() {
  std::unique_lock<std::mutex> lock(mu_);
  bool allow_delay = true;
  auto stall = [&](auto&& wait) {
    const uint64_t start = NowMicros();
    wait();
    ++stats_.stall_count;
    stats_.stall_micros += NowMicros() - start;
  };
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    const size_t l0_files = versions_->current()->files(0).size();
    if (allow_delay && l0_files >= static_cast<size_t>(options_.l0_slowdown_trigger)) {
      // Too many L0 files: slow this write by 1 ms, once, so compaction can
      // catch up before writes have to stop.
      stall([&] {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
      });
      allow_delay = false;
      continue;
    }
    const bool full = mem_->ApproximateMemoryUsage() >= options_.memtable_size;
    if (force_flush_ && !full && mem_->Empty()) {
      force_flush_ = false;  // A requested flush with nothing new to flush.
    }
    if (!full && !force_flush_) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed: wait for it.
      stall([&] { bg_cv_.wait(lock); });
      continue;
    }
    if (l0_files >= static_cast<size_t>(options_.l0_stop_trigger)) {
      stall([&] { bg_cv_.wait(lock); });
      continue;
    }

    // Switch to a new memtable and WAL. The old memtable becomes immutable
    // and the flush thread picks it up.
    const uint64_t number = versions_->NewFileNumber();
    std::unique_ptr<log::Writer> new_log;
    Status s = CreateLog(number, &new_log);
    if (!s.ok()) {
      return s;
    }
    s = log_->file()->Close();
    if (!s.ok()) {
      RecordBackgroundError(s);  // The old WAL may not hold all its records.
    }
    log_ = std::move(new_log);
    logfile_number_ = number;
    imm_ = std::move(mem_);
    mem_ = NewMemTable();
    committer_->SetTarget(log_.get(), mem_.get());
    force_flush_ = false;
    ++memtables_switched_;
    InstallSuperVersion();
    bg_cv_.notify_all();
    return bg_error_;
  }
}

Status DBImpl::Flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    force_flush_ = true;
  }
  // An empty write makes a group leader run MakeRoomForWrite(), which
  // switches memtables while no other write is in progress.
  WriteBatch empty;
  Status s = committer_->Write(WriteOptions{}, &empty);
  if (!s.ok()) {
    return s;
  }
  std::unique_lock<std::mutex> lock(mu_);
  const uint64_t target = memtables_switched_;
  bg_cv_.wait(lock, [&] { return memtables_flushed_ >= target || !bg_error_.ok(); });
  return bg_error_;
}

// ---- Read path -------------------------------------------------------------

SuperVersion* DBImpl::AcquireSuperVersion(ThreadSlot* slot) {
  // Acquire: pairs with the release in InstallSuperVersion(). If the
  // generation matches the cached one, no newer SuperVersion has been
  // installed, so the cached reference is current: no mutex, no refcount.
  const uint64_t generation = super_version_generation_.load(std::memory_order_acquire);
  if (slot->super_version != nullptr && slot->generation == generation) {
    slot->Add(Counter::kSuperVersionFastPath);
    return slot->super_version;
  }
  SuperVersion* old = slot->super_version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    super_version_->Ref();
    slot->super_version = super_version_;
    slot->generation = super_version_->generation;
  }
  if (old != nullptr) {
    old->Unref();  // Outside the mutex: may free a memtable.
  }
  slot->Add(Counter::kSuperVersionSlowPath);
  return slot->super_version;
}

void DBImpl::InstallSuperVersion() {
  auto sv = std::make_unique<SuperVersion>();
  sv->mem = mem_;
  sv->imm = imm_;
  sv->version = versions_->current();
  sv->generation = ++super_version_counter_;
  SuperVersion* old = super_version_;
  super_version_ = sv.release();  // Owned through its reference count.
  // Release: publishes the new SuperVersion before readers can observe its
  // generation (see AcquireSuperVersion and Get).
  super_version_generation_.store(super_version_->generation, std::memory_order_release);
  if (old != nullptr) {
    old->Unref();
  }
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  return GetImpl(options, key, value, nullptr);
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key, PinnableValue* value) {
  value->Reset();  // Release whatever it pinned before.
  return GetImpl(options, key, nullptr, value);
}

Status DBImpl::GetImpl(const ReadOptions& options, std::string_view key, std::string* copy,
                       PinnableValue* pinned) {
  // Load the visible sequence BEFORE checking the SuperVersion generation. A
  // write that went into a newer memtable was published after that
  // memtable's SuperVersion was installed. So if our sequence covers the
  // write, the acquire here makes the newer generation visible to the
  // check, and we take the slow path to the newer SuperVersion. In the
  // other order, a stale SuperVersion could be paired with a sequence that
  // includes writes it does not contain.
  const SequenceNumber visible = committer_->visible_sequence();
  const SequenceNumber sequence =
      options.snapshot != nullptr ? options.snapshot->sequence() : visible;
  ThreadSlot& slot = per_thread_.Local();
  SuperVersion* sv = AcquireSuperVersion(&slot);
  slot.Add(Counter::kGets);

  const LookupKey lkey(key, sequence);
  std::string_view found;
  Status s;
  if (sv->mem->Get(lkey, &found, &s) || (sv->imm != nullptr && sv->imm->Get(lkey, &found, &s))) {
    if (s.ok()) {
      if (copy != nullptr) {
        copy->assign(found);
      } else {
        // The value lives in a memtable: keep it alive through a reference
        // on the SuperVersion that holds that memtable.
        sv->Ref();
        Pin pin;
        pin.Add([](void* p, void* /*unused*/) { static_cast<SuperVersion*>(p)->Unref(); }, sv,
                nullptr);
        pinned->PinTo(found, std::move(pin));
      }
    }
    return s;
  }

  int tables_consulted = 0;
  TableReadStats table_stats;
  const TableReadOptions read_options{.fill_cache = options.fill_cache, .stats = &table_stats};
  s = sv->version->Get(
      lkey, table_cache_.get(), read_options,
      [&](std::string_view v, Pin& pin) {
        if (copy != nullptr) {
          copy->assign(v);
        } else {
          pinned->PinTo(v, std::move(pin));  // Holds the block and its table.
        }
      },
      &tables_consulted);
  slot.Add(Counter::kTablesConsulted, static_cast<uint64_t>(tables_consulted));
  slot.Add(Counter::kBloomChecks, table_stats.filter_checks);
  slot.Add(Counter::kBloomNegatives, table_stats.filter_negatives);
  slot.Add(Counter::kBlockCacheHits, table_stats.block_cache_hits);
  slot.Add(Counter::kBlockCacheMisses, table_stats.block_cache_misses);
  return s;
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  // As in GetImpl: the sequence first, then the (at least as new)
  // SuperVersion.
  const SequenceNumber visible = committer_->visible_sequence();
  const SequenceNumber sequence =
      options.snapshot != nullptr ? options.snapshot->sequence() : visible;
  SuperVersion* sv = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sv = super_version_;
    sv->Ref();  // Released by the iterator.
  }
  const TableReadOptions read_options{.fill_cache = options.fill_cache};
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(NewMemTableIterator(sv->mem.get()));
  if (sv->imm != nullptr) {
    children.push_back(NewMemTableIterator(sv->imm.get()));
  }
  const Version& v = *sv->version;
  for (const FilePtr& f : v.files(0)) {
    children.push_back(table_cache_->NewIterator(f->number, f->file_size, read_options));
  }
  for (int level = 1; level < v.num_levels(); ++level) {
    if (!v.files(level).empty()) {
      children.push_back(NewLevelIterator(&v.files(level), table_cache_.get(), read_options));
    }
  }
  return NewDBIterator(std::make_unique<MergingIterator<>>(std::move(children)), sequence, sv);
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mu_);
  return &snapshots_.emplace_back(Snapshot::PrivateTag{}, committer_->visible_sequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  std::lock_guard<std::mutex> lock(mu_);
  snapshots_.remove_if([snapshot](const Snapshot& s) { return &s == snapshot; });
}

DBStats DBImpl::GetStats() {
  std::lock_guard<std::mutex> lock(mu_);
  DBStats st;
  st.flushes = stats_.flushes;
  st.flush_micros = stats_.flush_micros;
  st.flush_bytes_written = stats_.flush_bytes_written;
  st.compactions = stats_.compactions;
  st.compaction_micros = stats_.compaction_micros;
  st.compaction_bytes_read = stats_.compaction_bytes_read;
  st.compaction_bytes_written = stats_.compaction_bytes_written;
  st.rate_limiter_wait_micros = stats_.rate_limiter_wait_micros;
  st.stall_count = stats_.stall_count;
  st.stall_micros = stats_.stall_micros;

  const auto current = versions_->current();
  for (int level = 0; level < current->num_levels(); ++level) {
    st.files_per_level.push_back(static_cast<int>(current->files(level).size()));
    st.bytes_per_level.push_back(current->LevelBytes(level));
    st.sst_bytes += current->LevelBytes(level);
    if (current->LevelBytes(level) > 0) {
      st.estimated_live_bytes = current->LevelBytes(level);  // Deepest non-empty wins.
    }
  }

  const CounterTotals t = per_thread_.Sum();
  auto get = [&t](Counter c) { return t[static_cast<size_t>(c)]; };
  st.super_version_fast_path = get(Counter::kSuperVersionFastPath);
  st.super_version_slow_path = get(Counter::kSuperVersionSlowPath);
  st.gets = get(Counter::kGets);
  st.tables_consulted = get(Counter::kTablesConsulted);
  st.bloom_checks = get(Counter::kBloomChecks);
  st.bloom_negatives = get(Counter::kBloomNegatives);
  st.block_cache_hits = get(Counter::kBlockCacheHits);
  st.block_cache_misses = get(Counter::kBlockCacheMisses);
  st.user_bytes_written = get(Counter::kUserBytesWritten);
  st.wal_bytes_written = get(Counter::kWalBytesWritten);
  st.write_groups = get(Counter::kWriteGroups);
  for (size_t i = 0; i < st.group_size_histogram.size(); ++i) {
    st.group_size_histogram[i] = t[static_cast<size_t>(Counter::kGroupSize1) + i];
  }

  auto ratio = [](uint64_t num, uint64_t den) {
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  };
  st.write_amplification =
      ratio(st.wal_bytes_written + st.flush_bytes_written + st.compaction_bytes_written,
            st.user_bytes_written);
  st.read_amplification = ratio(st.tables_consulted, st.gets);
  st.space_amplification = ratio(st.sst_bytes, st.estimated_live_bytes);
  return st;
}

// ---- Flush -------------------------------------------------------------------

Status DBImpl::BuildTable(const MemTable& mem, uint64_t number, FileMetaData* meta) {
  const std::string path = TableFileName(dbname_, number);
  meta->number = number;
  meta->file_size = 0;
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Create(path, &file);
  if (!s.ok()) {
    return s;
  }
  TableBuilder builder(table_options_, file.get());
  MemTable::Iterator iter = mem.NewIterator();
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    if (builder.NumEntries() == 0) {
      meta->smallest.assign(iter.key());
    }
    meta->largest.assign(iter.key());
    builder.Add(iter.key(), iter.value());
  }
  s = builder.Finish();
  if (s.ok()) {
    s = file->Sync();  // Durable before any MANIFEST edit names it.
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (s.ok() && builder.NumEntries() > 0) {
    meta->file_size = builder.FileSize();
  } else {
    (void)RemoveFile(path);
  }
  return s;
}

void DBImpl::FlushThreadMain() {
  PinCurrentThreadToCpus(options_.background_thread_cpus);
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [this] {
      return shutting_down_.load(std::memory_order_relaxed) || (imm_ != nullptr && bg_error_.ok());
    });
    if (shutting_down_.load(std::memory_order_relaxed)) {
      return;
    }
    Status s = FlushImmutable(&lock);
    if (!s.ok()) {
      RecordBackgroundError(s);
    }
    bg_cv_.notify_all();
  }
}

Status DBImpl::FlushImmutable(std::unique_lock<std::mutex>* lock) {
  const std::shared_ptr<MemTable> imm = imm_;
  const uint64_t number = versions_->NewFileNumber();
  pending_outputs_.insert(number);
  const uint64_t start = NowMicros();

  lock->unlock();
  FileMetaData meta;
  Status s = BuildTable(*imm, number, &meta);
  if (s.ok() && meta.file_size > 0) {
    s = SyncDirectory(dbname_);
  }
  lock->lock();

  if (s.ok()) {
    VersionEdit edit;
    if (meta.file_size > 0) {
      edit.AddFile(0, meta);
    }
    // The memtable switch put all of imm_'s writes into WALs older than the
    // current one, so those WALs become obsolete with this edit.
    edit.SetLogNumber(logfile_number_);
    versions_->SetLastSequence(committer_->visible_sequence());
    s = versions_->LogAndApply(&edit, lock);
  }
  pending_outputs_.erase(number);
  if (s.ok()) {
    imm_.reset();
    ++memtables_flushed_;
    ++stats_.flushes;
    stats_.flush_micros += NowMicros() - start;
    stats_.flush_bytes_written += meta.file_size;
    InstallSuperVersion();
    DeleteObsoleteFiles(lock);
  }
  return s;
}

// ---- Compaction ----------------------------------------------------------------

void DBImpl::CompactionThreadMain() {
  PinCurrentThreadToCpus(options_.background_thread_cpus);
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    std::unique_ptr<Compaction> c;
    bg_cv_.wait(lock, [&] {
      if (shutting_down_.load(std::memory_order_relaxed)) {
        return true;
      }
      if (!bg_error_.ok()) {
        return false;
      }
      c = versions_->PickCompaction(busy_levels_);
      return c != nullptr;
    });
    if (shutting_down_.load(std::memory_order_relaxed)) {
      return;
    }
    (void)RunCompaction(std::move(c), &lock);  // Errors are recorded inside.
  }
}

Status DBImpl::RunCompaction(std::unique_ptr<Compaction> c, std::unique_lock<std::mutex>* lock) {
  const int level = c->level();
  busy_levels_[level] = true;
  busy_levels_[level + 1] = true;
  Status s = DoCompactionWork(c.get(), lock);
  // The compaction holds its input Version, which keeps the input files
  // live. Drop it before looking for obsolete files, or this compaction's
  // own inputs would linger until the next flush or compaction.
  c.reset();
  busy_levels_[level] = false;
  busy_levels_[level + 1] = false;
  if (s.ok()) {
    DeleteObsoleteFiles(lock);
  } else if (!shutting_down_.load(std::memory_order_relaxed)) {
    RecordBackgroundError(s);
  }
  bg_cv_.notify_all();
  return s;
}

Status DBImpl::DoCompactionWork(Compaction* c, std::unique_lock<std::mutex>* lock) {
  const uint64_t start = NowMicros();
  // Versions newer than this may be visible to a live snapshot or a future
  // read. Versions at or below it are visible to every reader, so only the
  // newest of them is needed.
  const SequenceNumber smallest_snapshot =
      snapshots_.empty() ? committer_->visible_sequence() : snapshots_.front().sequence_;
  lock->unlock();

  std::vector<std::unique_ptr<InternalIterator>> children;
  const TableReadOptions read_options{.fill_cache = false};  // Don't evict the hot set.
  for (int which = 0; which < 2; ++which) {
    for (const FilePtr& f : c->inputs(which)) {
      children.push_back(table_cache_->NewIterator(f->number, f->file_size, read_options));
    }
  }
  MergingIterator<> input(std::move(children));

  std::vector<FileMetaData> outputs;
  std::unique_ptr<WritableFile> file;
  std::unique_ptr<TableBuilder> builder;
  uint64_t bytes_reported = 0;
  std::chrono::nanoseconds rate_wait{0};
  auto rate_limit = [&] {
    // Charge bytes as data blocks reach the file, so pacing is per block.
    if (builder->FileSize() > bytes_reported) {
      rate_wait += rate_limiter_->Request(builder->FileSize() - bytes_reported);
      bytes_reported = builder->FileSize();
    }
  };
  auto open_output = [&] {
    lock->lock();
    const uint64_t number = versions_->NewFileNumber();
    pending_outputs_.insert(number);
    lock->unlock();
    outputs.emplace_back().number = number;
    Status os = WritableFile::Create(TableFileName(dbname_, number), &file);
    if (os.ok()) {
      builder = std::make_unique<TableBuilder>(table_options_, file.get());
      bytes_reported = 0;
    }
    return os;
  };
  auto finish_output = [&] {
    Status fs = builder->Finish();
    rate_limit();
    outputs.back().file_size = builder->FileSize();
    if (fs.ok()) {
      fs = file->Sync();
    }
    if (fs.ok()) {
      fs = file->Close();
    }
    builder.reset();
    file.reset();
    return fs;
  };

  Status s;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  for (input.SeekToFirst(); s.ok() && input.Valid(); input.Next()) {
    if (shutting_down_.load(std::memory_order_relaxed)) {
      s = Status::IOError("shutting down");  // Not recorded as a background error.
      break;
    }
    const std::string_view key = input.key();
    ParsedInternalKey ikey;
    if (!ParseInternalKey(key, &ikey)) {
      s = Status::Corruption("corrupted internal key in compaction input");
      break;
    }
    if (!has_current_user_key || ikey.user_key != current_user_key) {
      // A new user key: the only place an output file may end, so a user
      // key's versions never span two files of a level.
      const bool stop = c->ShouldStopBefore(key);
      if (builder != nullptr && (stop || builder->FileSize() >= c->max_output_file_size())) {
        s = finish_output();
        if (!s.ok()) {
          break;
        }
      }
      current_user_key.assign(ikey.user_key);
      has_current_user_key = true;
      last_sequence_for_key = kMaxSequenceNumber;
    }

    // MVCC drop rules (spec §3), as in LevelDB:
    //  * A version is hidden if a newer version of the same key is at or
    //    below smallest_snapshot: every reader sees that newer one instead.
    //  * A tombstone at or below smallest_snapshot can go if no deeper level
    //    may hold the key: older versions in this compaction are hidden by it
    //    (rule 1), and there are none below.
    bool drop = false;
    if (last_sequence_for_key <= smallest_snapshot) {
      drop = true;
    } else if (ikey.type == ValueType::kDeletion && ikey.sequence <= smallest_snapshot &&
               c->IsBaseLevelForKey(ikey.user_key)) {
      drop = true;
    }
    last_sequence_for_key = ikey.sequence;
    if (drop) {
      continue;
    }

    if (builder == nullptr) {
      s = open_output();
      if (!s.ok()) {
        break;
      }
      outputs.back().smallest.assign(key);
    }
    outputs.back().largest.assign(key);
    builder->Add(key, input.value());
    rate_limit();
  }
  if (s.ok()) {
    s = input.status();
  }
  if (builder != nullptr) {
    Status fs = finish_output();
    if (s.ok()) {
      s = fs;
    }
  }
  if (s.ok() && !outputs.empty()) {
    s = SyncDirectory(dbname_);
  }

  lock->lock();
  if (s.ok()) {
    VersionEdit* edit = c->edit();
    c->AddInputDeletions(edit);
    for (const FileMetaData& meta : outputs) {
      edit->AddFile(c->output_level(), meta);
    }
    s = versions_->LogAndApply(edit, lock);
  }
  for (const FileMetaData& meta : outputs) {
    pending_outputs_.erase(meta.number);
  }
  if (s.ok()) {
    ++stats_.compactions;
    stats_.compaction_micros += NowMicros() - start;
    stats_.compaction_bytes_read += c->InputBytes();
    for (const FileMetaData& meta : outputs) {
      stats_.compaction_bytes_written += meta.file_size;
    }
    stats_.rate_limiter_wait_micros += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(rate_wait).count());
    InstallSuperVersion();
  }
  return s;
}

Status DBImpl::CompactRange(std::string_view begin, std::string_view end) {
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  const std::string_view* begin_ptr = begin.empty() ? nullptr : &begin;
  const std::string_view* end_ptr = end.empty() ? nullptr : &end;

  std::unique_lock<std::mutex> lock(mu_);
  // Compact down to the deepest level holding data in the range, and at
  // least to L1, so data sitting only in L0 is compacted too (as in
  // LevelDB). L0 keeps every version a flush wrote; this is what drops the
  // stale ones.
  int max_level = 1;
  {
    const auto current = versions_->current();
    std::vector<FilePtr> inputs;
    for (int level = 1; level < current->num_levels(); ++level) {
      current->GetOverlappingInputs(level, begin_ptr, end_ptr, &inputs);
      if (!inputs.empty()) {
        max_level = level;
      }
    }
  }
  for (int level = 0; level < max_level; ++level) {
    while (true) {
      bg_cv_.wait(lock, [&] {
        return (!busy_levels_[level] && !busy_levels_[level + 1]) || !bg_error_.ok();
      });
      if (!bg_error_.ok()) {
        return bg_error_;
      }
      std::unique_ptr<Compaction> c = versions_->CompactRange(level, begin_ptr, end_ptr);
      if (c == nullptr) {
        break;
      }
      s = RunCompaction(std::move(c), &lock);
      if (!s.ok()) {
        return s;
      }
    }
  }
  return Status::OK();
}

Status DBImpl::WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lock(mu_);
  bg_cv_.wait(lock, [&] {
    if (!bg_error_.ok()) {
      return true;
    }
    if (imm_ != nullptr ||
        std::find(busy_levels_.begin(), busy_levels_.end(), true) != busy_levels_.end()) {
      return false;
    }
    const auto& scores = versions_->current()->compaction_scores();
    return std::all_of(scores.begin(), scores.end(), [](double s) { return s < 1; });
  });
  return bg_error_;
}

// ---- Housekeeping ----------------------------------------------------------------

void DBImpl::DeleteObsoleteFiles(std::unique_lock<std::mutex>* lock, bool recovering) {
  if (!bg_error_.ok()) {
    return;  // After an error the state is uncertain: keep every file.
  }
  // Live tables: referenced by some Version, or being written right now.
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  const uint64_t log_number = versions_->log_number();
  const uint64_t manifest_number = versions_->manifest_file_number();

  std::vector<std::string> children;
  if (!GetChildren(dbname_, &children).ok()) {
    return;
  }
  std::vector<std::pair<std::string, uint64_t>> doomed;  // (name, table number or 0)
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (!ParseFileName(name, &number, &type)) {
      continue;
    }
    bool keep = true;
    switch (type) {
      case FileType::kLogFile:
        keep = number >= log_number;
        break;
      case FileType::kDescriptorFile:
        keep = number >= manifest_number;
        break;
      case FileType::kTableFile:
        keep = live.count(number) > 0;
        break;
      case FileType::kTempFile:
        // A running LogAndApply may be writing one, so they are only
        // cleaned up at open.
        keep = !recovering;
        break;
      case FileType::kCurrentFile:
      case FileType::kLockFile:
        break;
    }
    if (!keep) {
      doomed.emplace_back(name, type == FileType::kTableFile ? number : 0);
    }
  }

  // New files always get numbers above any existing file, so nothing in
  // `doomed` can become live while we delete without the lock.
  lock->unlock();
  for (const auto& [name, table_number] : doomed) {
    if (table_number != 0) {
      table_cache_->Evict(table_number);
    }
    (void)RemoveFile(dbname_ + "/" + name);  // A concurrent deleter may win.
  }
  lock->lock();
}

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    bg_cv_.notify_all();
  }
}

}  // namespace talus
