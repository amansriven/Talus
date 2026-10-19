#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "test_util.h"
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
    return DBImpl::Open(options, dir_.path(), &db_);
  }

  std::string Get(std::string_view key, SequenceNumber snapshot = kMaxSequenceNumber) {
    std::string value;
    const Status s = db_->Get(GetOptions{.snapshot = snapshot}, key, &value);
    if (s.IsNotFound()) {
      return "NOT_FOUND";
    }
    return s.ok() ? value : "ERROR: " + s.ToString();
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
    int n = 0;
    for (int f : db_->GetInternalStats().files_per_level) {
      n += f;
    }
    return n;
  }

  test::ScopedTempDir dir_;
  std::unique_ptr<DBImpl> db_;
};

TEST_F(DBTest, OpenOptionsAndLocking) {
  Options no_create = SmallOptions();
  no_create.create_if_missing = false;
  EXPECT_TRUE(Open(no_create).IsInvalidArgument());
  ASSERT_TRUE(Open().ok());

  std::unique_ptr<DBImpl> second;
  EXPECT_TRUE(DBImpl::Open(SmallOptions(), dir_.path(), &second).IsBusy());

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
}

// Random puts, deletes and overwrites against a std::map model, with flushes
// and compactions running underneath and periodic reopens. Every key must
// match the model at every checkpoint.
TEST_F(DBTest, MatchesModelAcrossFlushesCompactionsAndReopens) {
  ASSERT_TRUE(Open().ok());
  std::map<std::string, std::string> model;
  std::mt19937 rng(301);
  auto check_all = [&] {
    for (int i = 0; i < 2000; ++i) {
      auto it = model.find(Key(i));
      ASSERT_EQ(Get(Key(i)), it == model.end() ? "NOT_FOUND" : it->second) << Key(i);
    }
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
      const DBInternalStats before_reopen = db_->GetInternalStats();
      flushes += before_reopen.flushes;
      compactions += before_reopen.compactions;
      ASSERT_TRUE(Open().ok());  // Reopen: recover from MANIFEST + WAL.
      check_all();
    }
  }
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  check_all();
  const DBInternalStats stats = db_->GetInternalStats();
  EXPECT_GT(flushes + stats.flushes, 10u);
  EXPECT_GT(compactions + stats.compactions, 0u);
  int deeper_files = 0;
  for (size_t level = 1; level < stats.files_per_level.size(); ++level) {
    deeper_files += stats.files_per_level[level];
  }
  EXPECT_GT(deeper_files, 0);
}

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
  EXPECT_EQ(db_->GetInternalStats().files_per_level[0], 1);
  EXPECT_EQ(Get("k"), "v");
  ASSERT_TRUE(db_->Flush().ok());  // Nothing new: no new file.
  EXPECT_EQ(db_->GetInternalStats().files_per_level[0], 1);
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

TEST_F(DBTest, SnapshotsPreserveOldVersionsThroughCompaction) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put({}, "k", "v1").ok());
  ASSERT_TRUE(db_->Put({}, "gone", "here").ok());
  const SequenceNumber snap = db_->AcquireSnapshot();
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
  const DBInternalStats stats = db_->GetInternalStats();
  EXPECT_GT(stats.stall_count, 0u);
  EXPECT_GT(stats.stall_micros, 0u);
  EXPECT_GT(stats.rate_limiter_wait_micros, 0u);
  EXPECT_LT(stats.files_per_level[0], o.l0_stop_trigger);
  EXPECT_EQ(Get(Key(4999)), std::string(200, 'x'));
}

TEST_F(DBTest, SteadyStateReadsUseTheLockFreeFastPath) {
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db_->Put({}, Key(i), "v").ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->WaitForBackgroundWork().ok());
  const DBInternalStats before = db_->GetInternalStats();
  std::thread reader([&] {  // A fresh thread: its first Get takes the slow path.
    for (int i = 0; i < 1000; ++i) {
      ASSERT_EQ(Get(Key(i % 500)), "v");
    }
  });
  reader.join();
  const DBInternalStats after = db_->GetInternalStats();
  EXPECT_EQ(after.super_version_slow_path - before.super_version_slow_path, 1u);
  EXPECT_EQ(after.super_version_fast_path - before.super_version_fast_path, 999u);
}

// Writers bump per-key counters while readers check that no key's value
// ever goes backwards, across memtable switches, flushes and compactions.
// Run under TSan this also checks the SuperVersion and write-path
// synchronization.
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
        const std::string value = Get(key);
        if (value == "NOT_FOUND") {
          ASSERT_EQ(last_seen.count(key), 0u) << "key vanished: " << key;
          continue;
        }
        ASSERT_EQ(value.size(), 108u) << value;
        const int counter = std::stoi(value.substr(0, 8));
        ASSERT_GE(counter, last_seen[key]) << "went backwards: " << key;
        last_seen[key] = counter;
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
      // The last write to key k was i = the largest i < kWritesPerWriter with
      // i % kKeysPerWriter == k.
      const int last = (kWritesPerWriter - 1) - ((kWritesPerWriter - 1 - k) % kKeysPerWriter);
      char expected[16];
      std::snprintf(expected, sizeof(expected), "%08d", last);
      EXPECT_EQ(Get("w" + std::to_string(w) + "_" + Key(k)).substr(0, 8), expected);
    }
  }
  EXPECT_GT(db_->GetInternalStats().flushes, 0u);
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

}  // namespace
}  // namespace talus
