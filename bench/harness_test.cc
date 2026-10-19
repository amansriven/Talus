#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "bench_common.h"

namespace talus::bench {
namespace {

TEST(BenchmarkHarness, PreservesRemainderAndHistogramSamples) {
  RunSpec spec;
  spec.threads = 3;
  spec.ops_per_thread = 10;
  spec.extra_ops = 2;
  auto r = RunWorkload(spec, [](int, uint64_t, std::mt19937_64&) { return true; });
  EXPECT_EQ(r.ops, 32U);
  EXPECT_EQ(r.latency.Count(), 32U);
  EXPECT_GT(r.seconds, 0);
  uint64_t samples = 0;
  for (const auto& s : r.per_second) samples += s.ops;
  EXPECT_EQ(samples, r.ops);
}

TEST(BenchmarkHarness, OpenLoopIncludesQueueingDelay) {
  RunSpec spec;
  spec.ops_per_thread = 10;
  spec.target_rate = 1000;
  auto r = RunWorkload(spec, [](int, uint64_t i, std::mt19937_64&) {
    if (i == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return true;
  });
  // The later operations are individually fast but still queued behind the
  // first one. Measuring only their service time would hide that delay.
  EXPECT_EQ(r.ops, 10U);
  EXPECT_GE(r.latency.Percentile(50), 20'000'000U);
}

TEST(BenchmarkHarness, DurationDoesNotWaitForFarFutureArrival) {
  RunSpec spec;
  spec.ops_per_thread = 10;
  spec.target_rate = 0.1;  // Ten seconds between scheduled operations.
  spec.max_seconds = 0.05;
  auto r = RunWorkload(spec, [](int, uint64_t, std::mt19937_64&) { return true; });
  EXPECT_EQ(r.ops, 1U);
  EXPECT_LT(r.seconds, 1);
}

TEST(BenchmarkHarness, CountsOperationFailures) {
  RunSpec spec;
  spec.ops_per_thread = 5;
  auto r = RunWorkload(spec, [](int, uint64_t i, std::mt19937_64&) { return i % 2 == 0; });
  EXPECT_EQ(r.errors, 2U);
  EXPECT_EQ(r.latency.Count(), 5U);
}

TEST(BenchmarkHistogram, PreservesLatenciesBeyondOneMinute) {
  Histogram h;
  h.Record(120'000'000'000ULL);
  EXPECT_EQ(h.Count(), 1U);
  EXPECT_GE(h.Max(), 120'000'000'000ULL);
}

TEST(BenchmarkPerfTotals, RequiresEveryWorkersCounter) {
  PerfTotals a, b, total;
  a.collected = b.collected = true;
  a.available[kCycles] = b.available[kCycles] = true;
  a.values[kCycles] = 100;
  b.values[kCycles] = 200;
  a.available[kInstructions] = true;
  a.values[kInstructions] = 500;
  total.Add(a);
  total.Add(b);
  EXPECT_EQ(total.values[kCycles], 300U);
  EXPECT_TRUE(total.available[kCycles]);
  EXPECT_FALSE(total.available[kInstructions]);
  EXPECT_EQ(total.ToJson(3).find("ipc"), std::string::npos);
}

TEST(BenchmarkEngines, RoundTripDeleteAndIterate) {
  for (const auto& name : AvailableEngines()) {
    SCOPED_TRACE(name);
    EngineConfig config;
    config.path = (std::filesystem::temp_directory_path() /
                   ("talus-engine-test-" + name + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                      .string();
    std::string error;
    auto engine = OpenEngine(name, config, &error);
    ASSERT_NE(engine, nullptr) << error;
    const std::string key("a\0b", 3), value("v\0x", 3);
    ASSERT_TRUE(engine->Put(key, value));
    std::string actual;
    EXPECT_EQ(engine->GetCopy(key, &actual), 1);
    EXPECT_EQ(actual, value);
    auto iter = engine->NewIterator();
    iter->Seek(key);
    ASSERT_TRUE(iter->Valid());
    EXPECT_EQ(iter->key(), key);
    EXPECT_EQ(iter->value(), value);
    EXPECT_TRUE(iter->ok());
    iter.reset();
    EXPECT_TRUE(engine->Delete(key));
    EXPECT_EQ(engine->Get(key), 0);
    engine.reset();
    config.fresh = false;
    engine = OpenEngine(name, config, &error);
    ASSERT_NE(engine, nullptr) << error;
    EXPECT_EQ(engine->Get(key), 0);
    engine.reset();
    std::filesystem::remove_all(config.path);
  }
}

}  // namespace
}  // namespace talus::bench
