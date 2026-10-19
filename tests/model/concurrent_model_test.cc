// Concurrent model test (spec §11).
//
// Writer threads each own a disjoint key range and a private std::map model,
// so each can check its own reads exactly while the others write. Background
// flush and compaction run throughout, plus a thread forcing extra flushes
// and range compactions. Snapshot readers check snapshot isolation:
//  * Atomicity: writers update key pairs (a_j, b_j) with the same version in
//    one WriteBatch, so any snapshot must see equal versions for each pair.
//  * Repeatable reads: reading a snapshot twice, with writes, flushes and
//    compactions in between, must give identical results.
//  * Monotonicity: a later snapshot never sees an older pair version.

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Talus/db.h"
#include "test_util.h"

namespace talus {
namespace {

constexpr int kWriters = 4;
constexpr int kKeysPerWriter = 300;
constexpr int kPairsPerWriter = 20;
constexpr int kOpsPerWriter = 4000;

std::string OwnKey(int writer, int k) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "w%d/key%05d", writer, k);
  return buf;
}

std::string PairKey(int writer, char side, int j) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "w%d/pair%c%03d", writer, side, j);
  return buf;
}

// All pair values under one snapshot, as "writer/pair -> version".
using PairState = std::map<std::string, std::string>;

PairState ReadPairs(DB* db, const Snapshot* snap) {
  PairState state;
  auto iter = db->NewIterator(ReadOptions{.snapshot = snap});
  for (int w = 0; w < kWriters; ++w) {
    const std::string prefix = "w" + std::to_string(w) + "/pair";
    for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
      state[std::string(iter->key())] = std::string(iter->value());
    }
  }
  EXPECT_TRUE(iter->status().ok()) << iter->status().ToString();
  return state;
}

TEST(ConcurrentModelTest, DisjointWritersWithSnapshotReaders) {
  test::ScopedTempDir dir;
  Options o;
  o.create_if_missing = true;
  o.memtable_size = 32 << 10;  // Constant memtable switches and flushes.
  o.block_size = 1024;
  o.target_file_size = 32 << 10;
  o.l1_max_bytes = 128 << 10;
  o.l0_compaction_trigger = 2;
  o.max_background_compactions = 2;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(o, dir.path(), &db).ok());

  std::atomic<int> writers_done{0};
  std::vector<std::map<std::string, std::string>> models(kWriters);
  std::vector<std::thread> threads;

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(static_cast<uint32_t>(w) + 100);
      auto& model = models[w];
      std::vector<int> pair_version(kPairsPerWriter, 0);
      for (int i = 0; i < kOpsPerWriter; ++i) {
        const int k = static_cast<int>(rng() % kKeysPerWriter);
        const std::string key = OwnKey(w, k);
        switch (rng() % 5) {
          case 0: {
            ASSERT_TRUE(db->Delete({}, key).ok());
            model.erase(key);
            break;
          }
          case 1: {
            // An atomic pair update: both sides get the same new version.
            const int j = static_cast<int>(rng() % kPairsPerWriter);
            const std::string version = std::to_string(++pair_version[j]);
            WriteBatch batch;
            batch.Put(PairKey(w, 'a', j), version);
            batch.Put(PairKey(w, 'b', j), version);
            ASSERT_TRUE(db->Write({}, &batch).ok());
            break;
          }
          default: {
            const std::string value = "w" + std::to_string(w) + "_" + std::to_string(i) +
                                      std::string(rng() % 100, 'x');
            ASSERT_TRUE(db->Put({}, key, value).ok());
            model[key] = value;
            break;
          }
        }
        // Read-your-writes on this writer's own keys: nobody else touches
        // them, so the private model is exact.
        const std::string probe = OwnKey(w, static_cast<int>(rng() % kKeysPerWriter));
        std::string value;
        const Status s = db->Get({}, probe, &value);
        auto it = model.find(probe);
        if (it == model.end()) {
          ASSERT_TRUE(s.IsNotFound()) << probe << ": " << s.ToString() << " " << value;
        } else {
          ASSERT_TRUE(s.ok()) << probe << ": " << s.ToString();
          ASSERT_EQ(value, it->second) << probe;
        }
      }
      writers_done.fetch_add(1, std::memory_order_relaxed);  // Relaxed: a stop signal.
    });
  }

  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&] {
      std::map<std::string, long> last_versions;  // Per pair key, across snapshots.
      int snapshots_checked = 0;
      while (writers_done.load(std::memory_order_relaxed) < kWriters) {
        const Snapshot* snap = db->GetSnapshot();
        const PairState first = ReadPairs(db.get(), snap);
        for (int w = 0; w < kWriters; ++w) {
          for (int j = 0; j < kPairsPerWriter; ++j) {
            auto a = first.find(PairKey(w, 'a', j));
            auto b = first.find(PairKey(w, 'b', j));
            ASSERT_EQ(a == first.end(), b == first.end()) << "half a pair at " << PairKey(w, 'a', j);
            if (a != first.end()) {
              ASSERT_EQ(a->second, b->second) << "torn pair at " << PairKey(w, 'a', j);
              const long version = std::stol(a->second);
              ASSERT_GE(version, last_versions[a->first]) << "pair went backwards";
              last_versions[a->first] = version;
            }
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // Let state move on.
        ASSERT_EQ(ReadPairs(db.get(), snap), first) << "snapshot read was not repeatable";
        // Point reads under the snapshot agree with its scan.
        for (const auto& [key, version] : first) {
          std::string value;
          ASSERT_TRUE(db->Get(ReadOptions{.snapshot = snap}, key, &value).ok());
          ASSERT_EQ(value, version);
        }
        db->ReleaseSnapshot(snap);
        ++snapshots_checked;
      }
      EXPECT_GT(snapshots_checked, 0);
    });
  }

  threads.emplace_back([&] {  // Extra churn: flushes and range compactions.
    std::mt19937 rng(9);
    while (writers_done.load(std::memory_order_relaxed) < kWriters) {
      if (rng() % 2 == 0) {
        ASSERT_TRUE(db->Flush().ok());
      } else {
        const int w = static_cast<int>(rng() % kWriters);
        ASSERT_TRUE(db->CompactRange(OwnKey(w, 0), OwnKey(w, kKeysPerWriter)).ok());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  for (auto& t : threads) {
    t.join();
  }

  // Final check: each writer's whole range matches its model exactly.
  ASSERT_TRUE(db->WaitForBackgroundWork().ok());
  auto iter = db->NewIterator({});
  for (int w = 0; w < kWriters; ++w) {
    const std::string prefix = "w" + std::to_string(w) + "/key";
    auto expected = models[w].begin();
    for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
      ASSERT_NE(expected, models[w].end()) << "extra key " << iter->key();
      ASSERT_EQ(iter->key(), expected->first);
      ASSERT_EQ(iter->value(), expected->second);
      ++expected;
    }
    EXPECT_EQ(expected, models[w].end()) << "missing keys for writer " << w;
  }
  const DBStats st = db->GetStats();
  EXPECT_GT(st.flushes, 5u);
  EXPECT_GT(st.compactions, 0u);
}

}  // namespace
}  // namespace talus
