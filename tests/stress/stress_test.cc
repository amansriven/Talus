// TSan stress test (spec §11): concurrent writers, readers, iterators and
// snapshots while flushes and compactions run. Its main assertion is that
// ThreadSanitizer reports nothing (a report fails the process); it also
// checks that every value read is well-formed and every scan is sorted.
//
// Duration: TALUS_STRESS_SECONDS (default 5).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Talus/db.h"
#include "test_util.h"

namespace talus {
namespace {

constexpr int kKeySpace = 5000;

std::string Key(uint32_t i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "s%05u", i % kKeySpace);
  return buf;
}

// Values embed their key, so readers can check what they get back.
std::string ValueFor(std::string_view key, uint32_t salt) {
  std::string v(key);
  v += ":" + std::to_string(salt);
  v.resize(v.size() + salt % 200, '.');
  return v;
}

bool WellFormed(std::string_view key, std::string_view value) {
  return value.size() > key.size() && value.substr(0, key.size()) == key &&
         value[key.size()] == ':';
}

TEST(StressTest, EverythingAtOnce) {
  const char* env = std::getenv("TALUS_STRESS_SECONDS");
  const auto duration = std::chrono::seconds(env != nullptr ? std::atoi(env) : 5);

  test::ScopedTempDir dir;
  Options o;
  o.create_if_missing = true;
  o.memtable_size = 64 << 10;
  o.block_size = 1024;
  o.target_file_size = 64 << 10;
  o.l1_max_bytes = 256 << 10;
  o.l0_compaction_trigger = 2;
  o.max_background_compactions = 2;    // Concurrent compactions too.
  o.block_cache_capacity = 256 << 10;  // Small: constant eviction.
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(o, dir.path(), &db).ok());

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> operations{0};
  auto running = [&] { return !stop.load(std::memory_order_relaxed); };  // Relaxed: a flag.
  auto count = [&] { operations.fetch_add(1, std::memory_order_relaxed); };
  std::vector<std::thread> threads;

  for (int t = 0; t < 3; ++t) {  // Writers: puts, deletes, batches; some synced.
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t));
      while (running()) {
        const std::string key = Key(rng());
        const WriteOptions wo{.sync = rng() % 500 == 0};
        switch (rng() % 4) {
          case 0:
            ASSERT_TRUE(db->Delete(wo, key).ok());
            break;
          case 1: {
            WriteBatch batch;
            for (int j = 0; j < 5; ++j) {
              const std::string k = Key(rng());
              batch.Put(k, ValueFor(k, rng()));
            }
            batch.Delete(Key(rng()));
            ASSERT_TRUE(db->Write(wo, &batch).ok());
            break;
          }
          default:
            ASSERT_TRUE(db->Put(wo, key, ValueFor(key, rng())).ok());
        }
        count();
      }
    });
  }
  for (int t = 0; t < 3; ++t) {  // Point readers, copying and pinned.
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(100 + t));
      std::string value;
      PinnableValue pinned;
      while (running()) {
        const std::string key = Key(rng());
        if (rng() % 2 == 0) {
          const Status s = db->Get({}, key, &value);
          ASSERT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
          ASSERT_TRUE(!s.ok() || WellFormed(key, value)) << key << " -> " << value;
        } else {
          const Status s = db->Get({.fill_cache = rng() % 4 != 0}, key, &pinned);
          ASSERT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
          ASSERT_TRUE(!s.ok() || WellFormed(key, pinned.value())) << key;
        }
        count();
      }
    });
  }
  for (int t = 0; t < 2; ++t) {  // Iterators, some on snapshots.
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(200 + t));
      while (running()) {
        const Snapshot* snap = rng() % 2 == 0 ? db->GetSnapshot() : nullptr;
        auto iter = db->NewIterator({.snapshot = snap});
        std::string prev;
        int steps = 0;
        for (iter->Seek(Key(rng())); iter->Valid() && steps < 200; iter->Next(), ++steps) {
          ASSERT_LT(prev, iter->key());
          ASSERT_TRUE(WellFormed(iter->key(), iter->value())) << iter->key();
          prev = iter->key();
        }
        ASSERT_TRUE(iter->status().ok()) << iter->status().ToString();
        iter.reset();
        if (snap != nullptr) {
          db->ReleaseSnapshot(snap);
        }
        count();
      }
    });
  }
  threads.emplace_back([&] {  // Snapshot churn plus snapshot point reads.
    std::mt19937 rng(300);
    std::vector<const Snapshot*> live;
    while (running()) {
      if (live.size() < 16 && rng() % 2 == 0) {
        live.push_back(db->GetSnapshot());
      } else if (!live.empty()) {
        const size_t i = rng() % live.size();
        db->ReleaseSnapshot(live[i]);
        live.erase(live.begin() + static_cast<long>(i));
      }
      if (!live.empty()) {
        std::string value;
        const std::string key = Key(rng());
        const Status s = db->Get({.snapshot = live[rng() % live.size()]}, key, &value);
        ASSERT_TRUE(s.ok() || s.IsNotFound());
      }
      count();
    }
    for (const Snapshot* s : live) {
      db->ReleaseSnapshot(s);
    }
  });
  threads.emplace_back([&] {  // Manual flushes and compactions.
    std::mt19937 rng(400);
    while (running()) {
      if (rng() % 3 == 0) {
        ASSERT_TRUE(db->Flush().ok());
      } else {
        const uint32_t a = rng() % kKeySpace;
        ASSERT_TRUE(db->CompactRange(Key(a), Key(a + 500)).ok());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      (void)db->GetStats();  // Sums per-thread counters while they change.
    }
  });

  std::this_thread::sleep_for(duration);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) {
    t.join();
  }
  ASSERT_TRUE(db->WaitForBackgroundWork().ok());
  const DBStats st = db->GetStats();
  RecordProperty("operations", std::to_string(operations.load()));
  EXPECT_GT(st.flushes, 0u);
  EXPECT_GT(st.compactions, 0u);
  EXPECT_GT(operations.load(), 1000u);
}

}  // namespace
}  // namespace talus
