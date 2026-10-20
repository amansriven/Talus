#include "Talus/db.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"
#include "util/file.h"
#include "util/filename.h"

namespace talus {
namespace {

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

// Small sizes so a few megabytes of writes exercise many flushes and
// compactions across several levels.
Options SmallOptions() {
  Options o;
  o.create_if_missing = true;
  o.memtable_size = 32 << 10;
  o.block_size = 1024;
  o.target_file_size = 32 << 10;
  o.l1_max_bytes = 128 << 10;
  o.l0_compaction_trigger = 2;
  o.l0_slowdown_trigger = 6;
  o.l0_stop_trigger = 10;
  o.block_cache_capacity = 1 << 20;
  return o;
}

class DBTest : public ::testing::Test {
 protected:
  Status Open(const Options& options = SmallOptions()) {
    db_.reset();
    return DB::Open(options, dir_.path(), &db_);
  }

  std::string Get(std::string_view key, const Snapshot* snapshot = nullptr) {
    std::string value;
    const Status s = db_->Get(ReadOptions{.snapshot = snapshot}, key, &value);
    if (s.IsNotFound()) {
      return "NOT_FOUND";
    }
    return s.ok() ? value : "ERROR: " + s.ToString();
  }

  // Every (key, value) an iterator yields, as "k=v " pairs.
  static std::string Scan(Iterator* iter) {
    std::string out;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      out += std::string(iter->key()) + "=" + std::string(iter->value()) + " ";
    }
    EXPECT_TRUE(iter->status().ok()) << iter->status().ToString();
    return out;
  }

  int CountFiles(FileType wanted) {
    std::vector<std::string> names;
    EXPECT_TRUE(GetChildren(dir_.path(), &names).ok());
    int n = 0;
    for (const auto& name : names) {
      uint64_t number = 0;
      FileType type{};
      n += ParseFileName(name, &number, &type) && type == wanted ? 1 : 0;
    }
    return n;
  }

  int TotalTableFiles() {
    const auto files = db_->GetStats().files_per_level;
    return std::accumulate(files.begin(), files.end(), 0);
  }

  test::ScopedTempDir dir_;
  std::unique_ptr<DB> db_;
};

// ---- Open, basic operations ------------------------------------------------

TEST_F(DBTest, OpenOptionsAndLocking) {
  Options no_create = SmallOptions();
  no_create.create_if_missing = false;
  EXPECT_TRUE(Open(no_create).IsInvalidArgument());
  ASSERT_TRUE(Open().ok());

  std::unique_ptr<DB> second;
  EXPECT_TRUE(DB::Open(SmallOptions(), dir_.path(), &second).IsBusy());

  Options exclusive = SmallOptions();
  exclusive.error_if_exists = true;
  db_.reset();
  EXPECT_TRUE(Open(exclusive).IsInvalidArgument());

  Options bad = SmallOptions();
  bad.l0_stop_trigger = 1;
  EXPECT_TRUE(Open(bad).IsInvalidArgument());
}

TEST_F(DBTest, PutGetDeleteOverwrite) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "a", "1").ok());
  ASSERT_TRUE(db_->Put({}, "b", "2").ok());
  EXPECT_EQ(Get("a"), "1");
  ASSERT_TRUE(db_->Put({}, "a", "3").ok());
  EXPECT_EQ(Get("a"), "3");
  ASSERT_TRUE(db_->Delete({}, "b").ok());
  EXPECT_EQ(Get("b"), "NOT_FOUND");
  EXPECT_EQ(Get("never"), "NOT_FOUND");
  ASSERT_TRUE(db_->Put({}, "", "empty key").ok());
  EXPECT_EQ(Get(""), "empty key");

  WriteBatch batch;
  batch.Put("x", "1");
  batch.Put("y", "2");
  batch.Delete("a");
  ASSERT_TRUE(db_->Write({.sync = true}, &batch).ok());
  EXPECT_EQ(Get("x"), "1");
  EXPECT_EQ(Get("y"), "2");
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_TRUE(db_->Write({}, nullptr).IsInvalidArgument());
}

// Random puts, deletes and overwrites against a std::map model, with flushes
// and compactions running underneath and periodic reopens. Point reads and
// full scans must match the model at every checkpoint.
TEST_F(DBTest, MatchesModelAcrossFlushesCompactionsAndReopens) {
  ASSERT_TRUE(Open().ok());
  std::map<std::string, std::string> model;
  std::mt19937 rng(301);
  auto check_all = [&] {
    for (int i = 0; i < 2000; ++i) {
      auto it = model.find(Key(i));
      ASSERT_EQ(Get(Key(i)), it == model.end() ? "NOT_FOUND" : it->second) << Key(i);
    }
    std::string expected;
    for (const auto& [k, v] : model) {
      expected += k + "=" + v + " ";
    }
    auto iter = db_->NewIterator({});
    ASSERT_EQ(Scan(iter.get()), expected);
  };
  uint64_t flushes = 0;  // Stats are per DB instance, so add them up across reopens.
  uint64_t compactions = 0;
  for (int op = 0; op < 30000; ++op) {
    const std::string key = Key(static_cast<int>(rng() % 2000));
    if (rng() % 5 == 0) {
      ASSERT_TRUE(db_->Delete({}, key).ok());
      model.erase(key);
    } else {
      const std::string value(100 + rng() % 200, static_cast<char>('a' + op % 26));
      ASSERT_TRUE(db_->Put({}, key, value).ok());
      model[key] = value;
    }
    if (op % 10000 == 9999) {
      check_all();
      const DBStats before_reopen = db_->GetStats();
      flushes += before_reopen.flushes;
      compactions += before_reopen.compactions;
      ASSERT_TRUE(Open().ok());  // Reopen: recover from MANIFEST + WAL.
      check_all();
    }
  }
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  check_all();
  const DBStats stats = db_->GetStats();
  EXPECT_GT(flushes + stats.flushes, 10u);
  EXPECT_GT(compactions + stats.compactions, 0u);
  const auto& files = stats.files_per_level;
  EXPECT_GT(std::accumulate(files.begin() + 1, files.end(), 0), 0);  // Data below L0.
}

// ---- Recovery and files -------------------------------------------------------

TEST_F(DBTest, RecoversUnflushedWritesFromTheWal) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "v" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(Open().ok());  // Destroying the DB does not flush the memtable.
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(Get(Key(i)), "v" + std::to_string(i));
  }
  // Recovery flushed the replayed WAL to L0 and deleted it.
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_GE(TotalTableFiles(), 1);
}

TEST_F(DBTest, ObsoleteFilesAreDeleted) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i % 3000), std::string(200, 'x')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  EXPECT_EQ(CountFiles(FileType::kTableFile), TotalTableFiles());
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(CountFiles(FileType::kDescriptorFile), 1);
  EXPECT_EQ(CountFiles(FileType::kTempFile), 0);
}

TEST_F(DBTest, FlushWritesL0) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "k", "v").ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(db_->GetStats().files_per_level[0], 1);
  EXPECT_EQ(Get("k"), "v");
  ASSERT_TRUE(db_->Flush().ok());  // Nothing new: no new file.
  EXPECT_EQ(db_->GetStats().files_per_level[0], 1);
}

TEST_F(DBTest, CompactRangeCompactsDataThatIsOnlyInL0) {
  Options o = SmallOptions();
  o.l0_compaction_trigger = 8;  // No background compaction: only CompactRange.
  o.l0_slowdown_trigger = 8;
  o.l0_stop_trigger = 12;
  ASSERT_TRUE(Open(o).ok());
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "v").ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_EQ(db_->GetStats().files_per_level[0], 1);
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  const DBStats st = db_->GetStats();
  EXPECT_EQ(st.files_per_level[0], 0);
  EXPECT_GE(st.files_per_level[1], 1);
  EXPECT_EQ(Get(Key(150)), "v");
}

TEST_F(DBTest, CompactRangeDropsDeletedDataAtTheBottom) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), std::string(100, 'v')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db_->Delete({}, Key(i)).ok());
  }
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  // Values are hidden by tombstones, and tombstones with nothing below them
  // are dropped: nothing remains.
  EXPECT_EQ(TotalTableFiles(), 0);
  EXPECT_EQ(Get(Key(5)), "NOT_FOUND");
}

TEST_F(DBTest, CorruptWalRecordIsDroppedOrFatalWhenParanoid) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "v").ok());  // One WAL record each.
  }
  db_.reset();
  std::vector<std::string> names;
  ASSERT_TRUE(GetChildren(dir_.path(), &names).ok());
  std::string wal;
  for (const auto& name : names) {
    uint64_t number = 0;
    FileType type{};
    if (ParseFileName(name, &number, &type) && type == FileType::kLogFile) {
      wal = dir_.path() + "/" + name;
    }
  }
  ASSERT_FALSE(wal.empty());
  {
    std::fstream f(wal, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(std::filesystem::file_size(wal) / 2));
    f.put('\x55');
  }
  Options paranoid = SmallOptions();
  paranoid.paranoid_checks = true;
  EXPECT_TRUE(Open(paranoid).IsCorruption());

  ASSERT_TRUE(Open().ok());  // Default: stop at the bad record.
  EXPECT_EQ(Get(Key(0)), "v");
  EXPECT_EQ(Get(Key(99)), "NOT_FOUND");
}

TEST_F(DBTest, MissingTableFileIsCorruption) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "k", "v").ok());
  ASSERT_TRUE(db_->Flush().ok());
  db_.reset();
  std::vector<std::string> names;
  ASSERT_TRUE(GetChildren(dir_.path(), &names).ok());
  for (const auto& name : names) {
    uint64_t number = 0;
    FileType type{};
    if (ParseFileName(name, &number, &type) && type == FileType::kTableFile) {
      ASSERT_TRUE(RemoveFile(dir_.path() + "/" + name).ok());
    }
  }
  EXPECT_TRUE(Open().IsCorruption());
}

// ---- Snapshots -----------------------------------------------------------------

TEST_F(DBTest, SnapshotsPreserveOldVersionsThroughCompaction) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "k", "v1").ok());
  ASSERT_TRUE(db_->Put({}, "gone", "here").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put({}, "k", "v2").ok());
  ASSERT_TRUE(db_->Delete({}, "gone").ok());
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  EXPECT_EQ(Get("k", snap), "v1");
  EXPECT_EQ(Get("gone", snap), "here");
  EXPECT_EQ(Get("k"), "v2");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");

  db_->ReleaseSnapshot(snap);
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  EXPECT_EQ(Get("k"), "v2");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
}

TEST_F(DBTest, ReleasingAMiddleSnapshotKeepsTheOthers) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "k", "1").ok());
  const Snapshot* s1 = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put({}, "k", "2").ok());
  const Snapshot* s2 = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put({}, "k", "3").ok());
  const Snapshot* s3 = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put({}, "k", "4").ok());
  db_->ReleaseSnapshot(s2);
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  EXPECT_EQ(Get("k", s1), "1");
  EXPECT_EQ(Get("k", s3), "3");
  EXPECT_EQ(Get("k"), "4");
  db_->ReleaseSnapshot(s1);
  db_->ReleaseSnapshot(s3);
}

// ---- Iterators ---------------------------------------------------------------------

TEST_F(DBTest, IteratorOverEmptyDb) {
  ASSERT_TRUE(Open().ok());
  auto iter = db_->NewIterator({});
  EXPECT_EQ(Scan(iter.get()), "");
  iter->Seek("anything");
  EXPECT_FALSE(iter->Valid());
}

// Data spread over the memtable, the immutable memtable, L0 and deeper
// levels must merge into one sorted view without deleted keys or stale
// versions, and Seek must land on the lower bound.
TEST_F(DBTest, IteratorMergesAllSourcesAndSeeks) {
  ASSERT_TRUE(Open().ok());
  std::map<std::string, std::string> model;
  std::mt19937 rng(7);
  for (int round = 0; round < 6; ++round) {
    for (int i = 0; i < 1500; ++i) {
      const std::string key = Key(static_cast<int>(rng() % 3000) * 2);  // Even keys only.
      if (rng() % 4 == 0) {
        ASSERT_TRUE(db_->Delete({}, key).ok());
        model.erase(key);
      } else {
        const std::string value = "r" + std::to_string(round) + "_" + std::to_string(i);
        ASSERT_TRUE(db_->Put({}, key, value).ok());
        model[key] = value;
      }
    }
    if (round == 2) {
      ASSERT_TRUE(db_->CompactRange("", "").ok());  // Push some data to deep levels.
    }
  }
  std::string expected;
  for (const auto& [k, v] : model) {
    expected += k + "=" + v + " ";
  }
  auto iter = db_->NewIterator({});
  EXPECT_EQ(Scan(iter.get()), expected);

  for (int i = 0; i < 300; ++i) {
    const std::string target = Key(static_cast<int>(rng() % 6100));  // Odd targets too.
    iter->Seek(target);
    auto lb = model.lower_bound(target);
    if (lb == model.end()) {
      EXPECT_FALSE(iter->Valid());
    } else {
      ASSERT_TRUE(iter->Valid()) << target;
      EXPECT_EQ(iter->key(), lb->first);
      EXPECT_EQ(iter->value(), lb->second);
    }
  }
}

// An iterator reads a fixed point in time: writes, flushes and compactions
// after its creation are invisible to it, and it stays valid through them.
TEST_F(DBTest, IteratorIsAStableSnapshot) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "old").ok());
  }
  auto iter = db_->NewIterator({});
  const Snapshot* snap = db_->GetSnapshot();
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), std::string(300, 'n')).ok());
  }
  ASSERT_TRUE(db_->Put({}, "zzz", "new key").ok());
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  int n = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++n) {
    ASSERT_EQ(iter->value(), "old");
  }
  EXPECT_EQ(n, 500);
  auto snap_iter = db_->NewIterator({.snapshot = snap});
  n = 0;
  for (snap_iter->SeekToFirst(); snap_iter->Valid(); snap_iter->Next(), ++n) {
    ASSERT_EQ(snap_iter->value(), "old");
  }
  EXPECT_EQ(n, 500);
  db_->ReleaseSnapshot(snap);
}

// ---- PinnableValue -------------------------------------------------------------------

// A pinned value must stay valid while the memtable it points into is
// flushed and compacted away. Under ASan, a use-after-free would fail.
TEST_F(DBTest, PinnedMemtableValueOutlivesFlushAndCompaction) {
  ASSERT_TRUE(Open().ok());
  const std::string expected(1000, 'm');
  ASSERT_TRUE(db_->Put({}, "pinned", expected).ok());
  PinnableValue value;
  ASSERT_TRUE(db_->Get({}, "pinned", &value).ok());
  ASSERT_TRUE(db_->Put({}, "pinned", "overwritten").ok());
  ASSERT_TRUE(db_->CompactRange("", "").ok());
  EXPECT_EQ(value.value(), expected);
  value.Reset();
  EXPECT_TRUE(value.value().empty());
}

TEST_F(DBTest, PinnedTableValueOutlivesEvictionAndCompaction) {
  Options o = SmallOptions();
  o.block_cache_capacity = 1;  // Every unpinned block is evicted immediately.
  ASSERT_TRUE(Open(o).ok());
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "value-" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  PinnableValue value;
  ASSERT_TRUE(db_->Get({}, Key(1234), &value).ok());
  EXPECT_EQ(value.value(), "value-1234");
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "replaced").ok());
  }
  ASSERT_TRUE(db_->CompactRange("", "").ok());  // The pinned value's file is deleted.
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  EXPECT_EQ(value.value(), "value-1234");

  // Reusing the PinnableValue releases the old pin and pins the new value.
  ASSERT_TRUE(db_->Get({}, Key(7), &value).ok());
  EXPECT_EQ(value.value(), "replaced");
  EXPECT_TRUE(db_->Get({}, "absent", &value).IsNotFound());
  EXPECT_TRUE(value.value().empty());
}

// ---- Statistics ------------------------------------------------------------------------

TEST_F(DBTest, StatsReportAmplificationAndCounters) {
  Options options = SmallOptions();
  options.use_mmap_reads = false;  // Reads go through the block cache, whose counters we check.
  ASSERT_TRUE(Open(options).ok());
  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i % 4000), std::string(150, 'v')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  for (int i = 0; i < 2000; ++i) {
    std::string value;
    ASSERT_TRUE(db_->Get({}, Key(i), &value).ok());
    // Inside the tables' key ranges, so only the bloom filter can rule it out.
    EXPECT_TRUE(db_->Get({}, Key(i) + "_absent", &value).IsNotFound());
  }
  const DBStats st = db_->GetStats();
  EXPECT_GT(st.user_bytes_written, 20000u * 150);
  EXPECT_GE(st.wal_bytes_written, st.user_bytes_written);
  EXPECT_GT(st.flush_bytes_written, 0u);
  EXPECT_GT(st.compaction_bytes_written, 0u);
  EXPECT_GT(st.compaction_bytes_read, 0u);
  EXPECT_GT(st.write_amplification, 2.0);  // WAL + flush + at least some compaction.
  EXPECT_EQ(st.gets, 4000u);
  EXPECT_GT(st.read_amplification, 0.0);
  EXPECT_GT(st.bloom_checks, 0u);
  EXPECT_GT(st.bloom_negatives, 1500u);  // Most absent keys are filtered out.
  EXPECT_GT(st.block_cache_hits + st.block_cache_misses, 0u);
  EXPECT_EQ(st.write_groups, std::accumulate(st.group_size_histogram.begin(),
                                             st.group_size_histogram.end(), uint64_t{0}));
  EXPECT_GE(st.write_groups, 1u);
  EXPECT_GE(st.space_amplification, 1.0);
  EXPECT_EQ(st.sst_bytes,
            std::accumulate(st.bytes_per_level.begin(), st.bytes_per_level.end(), uint64_t{0}));
}

TEST_F(DBTest, SteadyStateReadsUseTheLockFreeFastPath) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "v").ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  const DBStats before = db_->GetStats();
  std::thread reader([&] {  // A fresh thread: its first Get takes the slow path.
    for (int i = 0; i < 1000; ++i) {
      ASSERT_EQ(Get(Key(i % 500)), "v");
    }
  });
  reader.join();
  const DBStats after = db_->GetStats();
  EXPECT_EQ(after.super_version_slow_path - before.super_version_slow_path, 1u);
  EXPECT_EQ(after.super_version_fast_path - before.super_version_fast_path, 999u);
  EXPECT_EQ(after.gets - before.gets, 1000u);  // Exited thread's counters retained.
}

// ---- Background work and concurrency -------------------------------------------------------

TEST_F(DBTest, WritesStallWhileCompactionIsRateLimited) {
  Options o = SmallOptions();
  o.memtable_size = 16 << 10;
  o.l0_compaction_trigger = 2;
  o.l0_slowdown_trigger = 3;
  o.l0_stop_trigger = 5;
  o.compaction_rate_limit_bytes_per_sec = 2 << 20;
  ASSERT_TRUE(Open(o).ok());
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), std::string(200, 'x')).ok());
  }
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  const DBStats stats = db_->GetStats();
  EXPECT_GT(stats.stall_count, 0u);
  EXPECT_GT(stats.stall_micros, 0u);
  EXPECT_GT(stats.rate_limiter_wait_micros, 0u);
  EXPECT_LT(stats.files_per_level[0], o.l0_stop_trigger);
  EXPECT_EQ(Get(Key(4999)), std::string(200, 'x'));
}

// Writers bump per-key counters while readers (point reads and iterators)
// check that no key's value ever goes backwards, across memtable switches,
// flushes and compactions. Under TSan this also checks the SuperVersion,
// write-path and iterator synchronization.
TEST_F(DBTest, ConcurrentWritersAndReadersSeeMonotonicValues) {
  ASSERT_TRUE(Open().ok());
  constexpr int kWriters = 4;
  constexpr int kKeysPerWriter = 200;
  constexpr int kWritesPerWriter = 3000;
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kWritesPerWriter; ++i) {
        const std::string key = "w" + std::to_string(w) + "_" + Key(i % kKeysPerWriter);
        char value[16];
        std::snprintf(value, sizeof(value), "%08d", i);
        ASSERT_TRUE(db_->Put({}, key, std::string(value) + std::string(100, 'p')).ok());
      }
    });
  }
  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(static_cast<uint32_t>(r));
      std::map<std::string, int> last_seen;
      while (!done.load(std::memory_order_relaxed)) {  // Relaxed: a stop flag.
        const std::string key = "w" + std::to_string(rng() % kWriters) + "_" +
                                Key(static_cast<int>(rng() % kKeysPerWriter));
        PinnableValue pinned;
        const Status s = db_->Get({}, key, &pinned);
        if (s.IsNotFound()) {
          ASSERT_EQ(last_seen.count(key), 0u) << "key vanished: " << key;
          continue;
        }
        ASSERT_TRUE(s.ok()) << s.ToString();
        ASSERT_EQ(pinned.value().size(), 108u);
        const int counter = std::stoi(std::string(pinned.value().substr(0, 8)));
        ASSERT_GE(counter, last_seen[key]) << "went backwards: " << key;
        last_seen[key] = counter;
        if (rng() % 64 == 0) {  // Occasionally scan: keys sorted and unique.
          auto iter = db_->NewIterator({});
          std::string prev;
          for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            ASSERT_LT(prev, iter->key());
            prev = iter->key();
          }
          ASSERT_TRUE(iter->status().ok());
        }
      }
    });
  }
  for (int w = 0; w < kWriters; ++w) {
    threads[w].join();
  }
  done.store(true, std::memory_order_relaxed);
  threads[kWriters].join();
  threads[kWriters + 1].join();

  for (int w = 0; w < kWriters; ++w) {
    for (int k = 0; k < kKeysPerWriter; ++k) {
      // The last write to key k was the largest i < kWritesPerWriter with
      // i % kKeysPerWriter == k.
      const int last = (kWritesPerWriter - 1) - ((kWritesPerWriter - 1 - k) % kKeysPerWriter);
      char expected[16];
      std::snprintf(expected, sizeof(expected), "%08d", last);
      EXPECT_EQ(Get("w" + std::to_string(w) + "_" + Key(k)).substr(0, 8), expected);
    }
  }
  EXPECT_GT(db_->GetStats().flushes, 0u);
}

}  // namespace
}  // namespace talus
