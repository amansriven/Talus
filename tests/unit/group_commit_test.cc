#include "db/group_commit.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "db/log_replay.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "test_util.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace talus {
namespace {

// The value of `key` at `seq`, or "<absent>".
std::string GetAt(const MemTable& mem, std::string_view key, SequenceNumber seq) {
  LookupKey lkey(key, seq);
  std::string_view value;
  Status s;
  if (!mem.Get(lkey, &value, &s) || s.IsNotFound()) {
    return "<absent>";
  }
  return std::string(value);
}

std::string Key(int thread, int i, char part) {
  return "t" + std::to_string(thread) + "_" + std::to_string(i) + "_" + part;
}

// Parameters: (concurrent memtable writes, spin iterations). Spin 0 also
// disables yielding, forcing every wait through the atomic::wait blocking path.
class GroupCommitTest : public ::testing::TestWithParam<std::tuple<bool, int>> {
 protected:
  GroupCommitTest() {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(WritableFile::Create(log_path_, &file).ok());
    log_ = std::make_unique<log::Writer>(std::move(file));
    const int spin = std::get<1>(GetParam());
    committer_ = std::make_unique<GroupCommitter>(
        log_.get(), &mem_, /*last_sequence=*/0,
        WriteThread::WaitOptions{.spin_iterations = spin, .max_yield_micros = spin == 0 ? 0 : 100});
  }

  // Number of records in the WAL and total operations across them.
  void CountLog(size_t* records, uint64_t* ops) {
    ASSERT_TRUE(log_->file()->Flush().ok());
    std::unique_ptr<SequentialFile> file;
    ASSERT_TRUE(SequentialFile::Open(log_path_, &file).ok());
    log::Reader reader(std::move(file));
    std::string scratch;
    std::string_view record;
    *records = 0;
    *ops = 0;
    while (reader.ReadRecord(&record, &scratch) == log::Reader::Result::kRecord) {
      ++*records;
      *ops += DecodeFixed32(record.data() + 8);
    }
  }

  test::ScopedTempDir dir_;
  const std::string log_path_ = dir_.File("000001.log");
  std::unique_ptr<log::Writer> log_;
  MemTable mem_{std::get<0>(GetParam())};
  std::unique_ptr<GroupCommitter> committer_;
};

INSTANTIATE_TEST_SUITE_P(Modes, GroupCommitTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Values(0, 256)),
                         [](const auto& info) {
                           return std::string(std::get<0>(info.param) ? "Concurrent"
                                                                      : "SingleWriter") +
                                  "_Spin" + std::to_string(std::get<1>(info.param));
                         });

TEST_P(GroupCommitTest, SequentialWritesAreVisibleAndDurableInLog) {
  for (int i = 0; i < 100; ++i) {
    WriteBatch b;
    b.Put(Key(0, i, 'a'), std::to_string(i));
    if (i % 10 == 0) {
      b.Delete(Key(0, i - 1, 'a'));
    }
    ASSERT_TRUE(committer_->Write(WriteOptions{.sync = i % 25 == 0}, &b).ok());
  }
  EXPECT_EQ(committer_->visible_sequence(), 100u + 10u);
  EXPECT_EQ(GetAt(mem_, Key(0, 5, 'a'), kMaxSequenceNumber), "5");
  EXPECT_EQ(GetAt(mem_, Key(0, 9, 'a'), kMaxSequenceNumber), "<absent>");  // Deleted by i=10.

  // Replaying the WAL rebuilds the same state.
  ASSERT_TRUE(log_->file()->Flush().ok());
  MemTable replayed;
  SequenceNumber max_seq = 0;
  ASSERT_TRUE(ReplayLogFile(log_path_, true, &replayed, &max_seq).ok());
  EXPECT_EQ(max_seq, 110u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(GetAt(replayed, Key(0, i, 'a'), kMaxSequenceNumber),
              GetAt(mem_, Key(0, i, 'a'), kMaxSequenceNumber));
  }
}

// Many writers, plus readers that check atomicity: each batch writes keys
// _a and _b with the same value, so at any published sequence both or
// neither must be visible. The visible sequence must never go backwards.
TEST_P(GroupCommitTest, ConcurrentWritersAreAtomicAndComplete) {
  constexpr int kWriters = 8;
  constexpr int kPerWriter = 1500;
  constexpr int kReaders = 2;
  std::atomic<bool> done{false};

  std::vector<std::thread> threads;
  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&, t] {
      WriteBatch b;
      for (int i = 0; i < kPerWriter; ++i) {
        b.Clear();
        const std::string value = std::to_string(t) + ":" + std::to_string(i);
        b.Put(Key(t, i, 'a'), value);
        b.Put(Key(t, i, 'b'), value);
        Status s = committer_->Write(WriteOptions{}, &b);
        ASSERT_TRUE(s.ok()) << s.ToString();
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(static_cast<uint32_t>(r));
      SequenceNumber last_seen = 0;
      // Relaxed: only a stop signal.
      while (!done.load(std::memory_order_relaxed)) {
        const SequenceNumber visible = committer_->visible_sequence();
        ASSERT_GE(visible, last_seen);
        last_seen = visible;
        const auto t = static_cast<int>(rng() % kWriters);
        const auto i = static_cast<int>(rng() % kPerWriter);
        const std::string a = GetAt(mem_, Key(t, i, 'a'), visible);
        const std::string b = GetAt(mem_, Key(t, i, 'b'), visible);
        ASSERT_EQ(a, b) << "batch half-visible at sequence " << visible;
      }
    });
  }
  for (int t = 0; t < kWriters; ++t) {
    threads[t].join();
  }
  done.store(true, std::memory_order_relaxed);
  for (int r = 0; r < kReaders; ++r) {
    threads[kWriters + r].join();
  }

  constexpr uint64_t kTotalOps = uint64_t{kWriters} * kPerWriter * 2;
  EXPECT_EQ(committer_->visible_sequence(), kTotalOps);
  for (int t = 0; t < kWriters; ++t) {
    for (int i = 0; i < kPerWriter; ++i) {
      ASSERT_EQ(GetAt(mem_, Key(t, i, 'a'), kMaxSequenceNumber),
                std::to_string(t) + ":" + std::to_string(i));
    }
  }
  size_t records = 0;
  uint64_t ops = 0;
  CountLog(&records, &ops);
  EXPECT_EQ(ops, kTotalOps);
  EXPECT_LE(records, size_t{kWriters} * kPerWriter);
}

// fsync takes milliseconds, so concurrent sync writers must share syncs:
// there must be fewer WAL records (one per group) than writes.
TEST_P(GroupCommitTest, SyncWritesAreGrouped) {
  constexpr int kWriters = 8;
  constexpr int kPerWriter = 20;
  std::vector<std::thread> threads;
  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerWriter; ++i) {
        WriteBatch b;
        b.Put(Key(t, i, 'a'), "v");
        ASSERT_TRUE(committer_->Write(WriteOptions{.sync = true}, &b).ok());
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  size_t records = 0;
  uint64_t ops = 0;
  CountLog(&records, &ops);
  EXPECT_EQ(ops, uint64_t{kWriters} * kPerWriter);
  EXPECT_LT(records, size_t{kWriters} * kPerWriter);
}

TEST_P(GroupCommitTest, LogFailureIsStickyAndAppliesNothing) {
  WriteBatch ok_batch;
  ok_batch.Put("before", "1");
  ASSERT_TRUE(committer_->Write(WriteOptions{}, &ok_batch).ok());

  ASSERT_TRUE(log_->file()->Close().ok());  // Later writes fail with EBADF.
  for (int i = 0; i < 2; ++i) {
    WriteBatch b;
    b.Put("after", "2");
    EXPECT_TRUE(committer_->Write(WriteOptions{}, &b).IsIOError());
  }
  EXPECT_EQ(committer_->visible_sequence(), 1u);
  EXPECT_EQ(GetAt(mem_, "after", kMaxSequenceNumber), "<absent>");
  EXPECT_EQ(GetAt(mem_, "before", kMaxSequenceNumber), "1");
}

}  // namespace
}  // namespace talus
