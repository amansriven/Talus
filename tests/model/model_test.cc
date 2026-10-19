// Model-based randomized test (spec §11).
//
// A seeded generator produces a sequence of operations: Put, Delete, atomic
// WriteBatch, Get, Scan, Snapshot (take/release/read), Flush, CompactRange
// and Reopen. They run against both the DB and a std::map model, with
// snapshots modeled as frozen copies of the map, and every read must match
// exactly. On a mismatch the test writes the seed and the executed operations
// to a file; `--replay FILE` re-executes exactly those operations, which
// reproduces the failure deterministically. Background flush and compaction
// timing varies between runs, but the expected results depend only on the
// operation sequence.
//
// Usage: talus_model_test [--ops N] [--seed S] [--replay FILE] [--dir PATH]
//                         [--inject-mismatch-at N]
// The last flag makes the first read at or after op N report a mismatch. It
// exists to test the failure log and replay path itself.

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Talus/db.h"

namespace talus {
namespace {

constexpr int kKeySpace = 4000;
constexpr int kSnapshotSlots = 8;

enum class OpType : int {
  kPut,
  kDelete,
  kBatch,
  kGet,
  kScan,
  kTakeSnapshot,
  kReleaseSnapshot,
  kSnapshotGet,
  kFlush,
  kCompactRange,
  kReopen,
};

// The meaning of a, b, c depends on the type (see Execute()).
struct Op {
  OpType type;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
};

std::string Key(uint32_t i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "k%06u", i % kKeySpace);
  return buf;
}

// A deterministic value: its length and contents depend only on the seed.
std::string Value(uint32_t seed) {
  std::string v = "v" + std::to_string(seed) + "_";
  v.resize(10 + seed % 150, static_cast<char>('a' + seed % 26));
  return v;
}

Op Generate(std::mt19937_64& rng) {
  const auto pick = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  const uint32_t r = pick(100000);
  Op op{};
  if (r < 40000) {
    op = {OpType::kPut, pick(kKeySpace), pick(1u << 30)};
  } else if (r < 50000) {
    op = {OpType::kDelete, pick(kKeySpace)};
  } else if (r < 58000) {
    op = {OpType::kBatch, pick(kKeySpace), 1 + pick(6), pick(1u << 30)};
  } else if (r < 83000) {
    op = {OpType::kGet, pick(kKeySpace)};
  } else if (r < 88000) {
    op = {OpType::kScan, pick(kKeySpace), 1 + pick(40), pick(kSnapshotSlots + 1)};
  } else if (r < 88300) {
    op = {OpType::kTakeSnapshot, pick(kSnapshotSlots)};
  } else if (r < 88600) {
    op = {OpType::kReleaseSnapshot, pick(kSnapshotSlots)};
  } else if (r < 99780) {
    op = {OpType::kSnapshotGet, pick(kSnapshotSlots), pick(kKeySpace)};
  } else if (r < 99930) {
    op = {OpType::kFlush};
  } else if (r < 99980) {
    const uint32_t lo = pick(kKeySpace);
    op = {OpType::kCompactRange, lo, lo + pick(kKeySpace / 4), pick(4)};
  } else {
    op = {OpType::kReopen};
  }
  return op;
}

std::string FormatOp(const Op& op) {
  std::ostringstream out;
  out << static_cast<int>(op.type) << ' ' << op.a << ' ' << op.b << ' ' << op.c;
  return out.str();
}

std::optional<Op> ParseOp(const std::string& line) {
  std::istringstream in(line);
  int type = 0;
  Op op{};
  if (!(in >> type >> op.a >> op.b >> op.c) || type < 0 ||
      type > static_cast<int>(OpType::kReopen)) {
    return std::nullopt;
  }
  op.type = static_cast<OpType>(type);
  return op;
}

using Model = std::map<std::string, std::string>;

class Runner {
 public:
  explicit Runner(std::string dir) : dir_(std::move(dir)) {}
  ~Runner() { CloseDB(); }

  bool Open() {
    Options o;
    o.create_if_missing = true;
    o.memtable_size = 64 << 10;
    o.block_size = 1024;
    o.target_file_size = 64 << 10;
    o.l1_max_bytes = 256 << 10;
    o.l0_compaction_trigger = 2;
    o.l0_slowdown_trigger = 8;
    o.l0_stop_trigger = 12;
    o.block_cache_capacity = 1 << 20;
    o.max_background_compactions = 2;
    const Status s = DB::Open(o, dir_, &db_);
    if (!s.ok()) {
      error_ = "Open failed: " + s.ToString();
    }
    return s.ok();
  }

  // Runs one op; returns false (with error()) on any mismatch.
  bool Execute(const Op& op) {
    switch (op.type) {
      case OpType::kPut:
        return Check(db_->Put({}, Key(op.a), Value(op.b)), "Put") &&
               (model_[Key(op.a)] = Value(op.b), true);
      case OpType::kDelete:
        return Check(db_->Delete({}, Key(op.a)), "Delete") && (model_.erase(Key(op.a)), true);
      case OpType::kBatch: {
        // Mixed puts and deletes, applied atomically.
        WriteBatch batch;
        std::vector<std::pair<std::string, std::optional<std::string>>> changes;
        for (uint32_t j = 0; j < op.b; ++j) {
          const std::string key = Key(op.a + j * 7);
          if ((op.c + j) % 3 == 0) {
            batch.Delete(key);
            changes.emplace_back(key, std::nullopt);
          } else {
            batch.Put(key, Value(op.c + j));
            changes.emplace_back(key, Value(op.c + j));
          }
        }
        if (!Check(db_->Write({}, &batch), "Write")) {
          return false;
        }
        for (auto& [key, value] : changes) {  // In order: a later op on a key wins.
          if (value) {
            model_[key] = std::move(*value);
          } else {
            model_.erase(key);
          }
        }
        return true;
      }
      case OpType::kGet:
        return CompareGet(Key(op.a), nullptr, model_);
      case OpType::kSnapshotGet:
        return snapshots_[op.a] == nullptr ||
               CompareGet(Key(op.b), snapshots_[op.a], snapshot_models_[op.a]);
      case OpType::kScan: {
        const bool use_snapshot = op.c > 0 && snapshots_[op.c - 1] != nullptr;
        const Snapshot* snap = use_snapshot ? snapshots_[op.c - 1] : nullptr;
        return CompareScan(Key(op.a), op.b, snap, use_snapshot ? snapshot_models_[op.c - 1] : model_);
      }
      case OpType::kTakeSnapshot:
        ReleaseSlot(op.a);
        snapshots_[op.a] = db_->GetSnapshot();
        snapshot_models_[op.a] = model_;
        return true;
      case OpType::kReleaseSnapshot:
        ReleaseSlot(op.a);
        return true;
      case OpType::kFlush:
        return Check(db_->Flush(), "Flush");
      case OpType::kCompactRange: {
        // c's bits choose unbounded ends, to cover full-range compactions.
        const std::string begin = (op.c & 1) != 0 ? "" : Key(op.a);
        const std::string end = (op.c & 2) != 0 ? "" : Key(std::min<uint32_t>(op.b, kKeySpace - 1));
        return Check(db_->CompactRange(begin, end), "CompactRange");
      }
      case OpType::kReopen:
        CloseDB();  // Snapshots do not survive a reopen.
        return Open();
    }
    return false;
  }

  const std::string& error() const { return error_; }

 private:
  void ReleaseSlot(uint32_t slot) {
    if (snapshots_[slot] != nullptr) {
      db_->ReleaseSnapshot(snapshots_[slot]);
      snapshots_[slot] = nullptr;
      snapshot_models_[slot].clear();
    }
  }

  void CloseDB() {
    if (db_ == nullptr) {
      return;
    }
    for (uint32_t slot = 0; slot < kSnapshotSlots; ++slot) {
      ReleaseSlot(slot);
    }
    db_.reset();
  }

  bool Check(const Status& s, const char* what) {
    if (!s.ok()) {
      error_ = std::string(what) + " failed: " + s.ToString();
    }
    return s.ok();
  }

  bool CompareGet(const std::string& key, const Snapshot* snap, const Model& model) {
    std::string value;
    const Status s = db_->Get(ReadOptions{.snapshot = snap}, key, &value);
    auto it = model.find(key);
    if (it == model.end() ? s.IsNotFound() : (s.ok() && value == it->second)) {
      return true;
    }
    error_ = "Get(" + key + (snap != nullptr ? ", snapshot" : "") + ") returned " +
             (s.ok() ? "\"" + value + "\"" : s.ToString()) + ", model has " +
             (it == model.end() ? "nothing" : "\"" + it->second + "\"");
    return false;
  }

  bool CompareScan(const std::string& start, uint32_t count, const Snapshot* snap,
                   const Model& model) {
    auto iter = db_->NewIterator(ReadOptions{.snapshot = snap});
    iter->Seek(start);
    auto it = model.lower_bound(start);
    for (uint32_t i = 0; i < count; ++i, ++it, iter->Next()) {
      const bool db_valid = iter->Valid();
      if (db_valid != (it != model.end()) ||
          (db_valid && (iter->key() != it->first || iter->value() != it->second))) {
        error_ = "Scan from " + start + " entry " + std::to_string(i) + ": DB has " +
                 (db_valid ? std::string(iter->key()) : "end") + ", model has " +
                 (it != model.end() ? it->first : "end");
        return false;
      }
      if (!db_valid) {
        break;
      }
    }
    if (!iter->status().ok()) {
      error_ = "Scan error: " + iter->status().ToString();
      return false;
    }
    return true;
  }

  const std::string dir_;
  std::unique_ptr<DB> db_;
  Model model_;
  std::array<const Snapshot*, kSnapshotSlots> snapshots_{};
  std::array<Model, kSnapshotSlots> snapshot_models_;
  std::string error_;
};

int Run(int argc, char** argv) {
  uint64_t num_ops = 1'000'000;
  uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::string replay_file;
  std::string dir;
  uint64_t inject_at = UINT64_MAX;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--ops") {
      num_ops = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (flag == "--seed") {
      seed = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (flag == "--replay") {
      replay_file = argv[i + 1];
    } else if (flag == "--dir") {
      dir = argv[i + 1];
    } else if (flag == "--inject-mismatch-at") {
      inject_at = std::strtoull(argv[i + 1], nullptr, 10);
    } else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 2;
    }
  }

  // The operations: generated from the seed, or read back from a failure log.
  std::vector<Op> ops;
  if (!replay_file.empty()) {
    std::ifstream in(replay_file);
    std::string line;
    std::getline(in, line);  // "seed N"
    std::sscanf(line.c_str(), "seed %" SCNu64, &seed);
    while (std::getline(in, line)) {
      if (auto op = ParseOp(line)) {
        ops.push_back(*op);
      }
    }
    std::printf("replaying %zu ops from %s\n", ops.size(), replay_file.c_str());
  } else {
    std::mt19937_64 rng(seed);
    ops.reserve(num_ops);
    for (uint64_t i = 0; i < num_ops; ++i) {
      ops.push_back(Generate(rng));
    }
    std::printf("model test: seed %" PRIu64 ", %zu ops\n", seed, ops.size());
  }

  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("talus_model_" + std::to_string(seed))).string();
  }
  std::filesystem::remove_all(dir);
  auto runner = std::make_unique<Runner>(dir);
  if (!runner->Open()) {
    std::fprintf(stderr, "%s\n", runner->error().c_str());
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < ops.size(); ++i) {
    const bool is_read = ops[i].type == OpType::kGet || ops[i].type == OpType::kScan ||
                         ops[i].type == OpType::kSnapshotGet;
    const bool injected = i >= inject_at && is_read;
    if (injected || !runner->Execute(ops[i])) {
      const std::string log = "talus_model_failure_" + std::to_string(seed) + ".txt";
      std::ofstream out(log);
      out << "seed " << seed << "\n";
      for (size_t j = 0; j <= i; ++j) {
        out << FormatOp(ops[j]) << "\n";
      }
      std::fprintf(stderr,
                   "MISMATCH at op %zu (%s): %s\n"
                   "Wrote %zu ops to %s. Reproduce with: --replay %s\n"
                   "DB left in %s\n",
                   i, FormatOp(ops[i]).c_str(),
                   injected ? "injected mismatch" : runner->error().c_str(), i + 1, log.c_str(),
                   log.c_str(), dir.c_str());
      return 1;
    }
    if ((i + 1) % 250000 == 0) {
      std::printf("  %zu ops ok\n", i + 1);
      std::fflush(stdout);
    }
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  runner.reset();
  std::filesystem::remove_all(dir);
  std::printf("PASSED: %zu ops in %.1f s\n", ops.size(), secs);
  return 0;
}

}  // namespace
}  // namespace talus

int main(int argc, char** argv) { return talus::Run(argc, argv); }
