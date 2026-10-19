// Crash harness (spec §11).
//
// Each iteration forks a child process that opens the DB and writes
// sequential batches with sync=true, reporting each acknowledged batch
// number to the parent through a pipe. After a random delay the parent
// SIGKILLs the child, reopens the DB, and checks:
//  * every acknowledged batch is present, with the right values (durability);
//  * no batch is partially present (atomicity);
//  * the batches present form a prefix of the sequence, and the "last_batch"
//    counter written by every batch names its last one (ordering).
// The memtable is small, so kills regularly land mid-flush, mid-compaction
// and mid-MANIFEST-write. The next iteration continues on the same DB.
//
// Usage: talus_crash_test [--iterations N] [--seed S] [--dir PATH]
// (default 100 iterations; the long run is --iterations 10000)

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "Talus/db.h"

namespace talus {
namespace {

constexpr int kKeysPerBatch = 4;

Options CrashOptions() {
  Options o;
  o.create_if_missing = true;
  o.memtable_size = 32 << 10;
  o.target_file_size = 32 << 10;
  o.l1_max_bytes = 128 << 10;
  o.l0_compaction_trigger = 2;
  return o;
}

std::string BatchKey(uint64_t batch, int j) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "b%010" PRIu64 "_%d", batch, j);
  return buf;
}

std::string BatchValue(uint64_t batch, int j) {
  std::string v = "batch " + std::to_string(batch) + " key " + std::to_string(j) + " ";
  v.resize(200, static_cast<char>('a' + (batch + static_cast<uint64_t>(j)) % 26));
  return v;
}

[[noreturn]] void RunChild(const std::string& dir, uint64_t first_batch, int ack_fd) {
  std::unique_ptr<DB> db;
  if (!DB::Open(CrashOptions(), dir, &db).ok()) {
    _exit(3);
  }
  WriteBatch batch;
  for (uint64_t b = first_batch;; ++b) {
    batch.Clear();
    for (int j = 0; j < kKeysPerBatch; ++j) {
      batch.Put(BatchKey(b, j), BatchValue(b, j));
    }
    batch.Put("last_batch", std::to_string(b));
    if (!db->Write(WriteOptions{.sync = true}, &batch).ok()) {
      _exit(4);
    }
    // Durable now: acknowledge it. A partial write to the pipe can only
    // happen if we are killed mid-write, and the parent ignores partial acks.
    if (write(ack_fd, &b, sizeof(b)) != static_cast<ssize_t>(sizeof(b))) {
      _exit(5);
    }
  }
}

// Reads acks until `deadline` (or EOF). Returns the highest one seen, or -1.
int64_t ReadAcks(int fd, std::chrono::steady_clock::time_point deadline, std::string* partial) {
  int64_t max_ack = -1;
  char buf[4096];
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    int timeout_ms = -1;  // time_point::max(): block until EOF.
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      timeout_ms =
          now >= deadline
              ? 0
              : static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }
    pollfd p{fd, POLLIN, 0};
    const int ready = poll(&p, 1, timeout_ms);
    if (ready <= 0) {
      return max_ack;  // Deadline reached.
    }
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      return max_ack;  // EOF: the child is gone and the pipe is drained.
    }
    partial->append(buf, static_cast<size_t>(n));
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= partial->size(); i += sizeof(uint64_t)) {
      uint64_t b = 0;
      std::memcpy(&b, partial->data() + i, sizeof(b));
      max_ack = std::max<int64_t>(max_ack, static_cast<int64_t>(b));
    }
    partial->erase(0, i);
  }
}

// Verifies batches [check_from, ...) after a crash. Returns the number of
// batches present (the next batch to write), or -1 on failure.
int64_t Verify(const std::string& dir, uint64_t check_from, int64_t max_acked, int iteration) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(CrashOptions(), dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "iteration %d: reopen failed: %s\n", iteration, s.ToString().c_str());
    return -1;
  }
  // Batches before check_from were verified by earlier iterations; spot-check
  // a few of them so data loss there would still be noticed.
  std::mt19937_64 rng(static_cast<uint64_t>(iteration));
  for (int i = 0; i < 20 && check_from > 0; ++i) {
    const uint64_t b = rng() % check_from;
    std::string value;
    if (!db->Get({}, BatchKey(b, 0), &value).ok() || value != BatchValue(b, 0)) {
      std::fprintf(stderr, "iteration %d: old batch %" PRIu64 " lost\n", iteration, b);
      return -1;
    }
  }
  uint64_t next = check_from;
  for (uint64_t b = check_from;; ++b) {
    int present = 0;
    for (int j = 0; j < kKeysPerBatch; ++j) {
      std::string value;
      const Status gs = db->Get({}, BatchKey(b, j), &value);
      if (gs.ok()) {
        if (value != BatchValue(b, j)) {
          std::fprintf(stderr, "iteration %d: batch %" PRIu64 " key %d has a wrong value\n",
                       iteration, b, j);
          return -1;
        }
        ++present;
      } else if (!gs.IsNotFound()) {
        std::fprintf(stderr, "iteration %d: Get failed: %s\n", iteration, gs.ToString().c_str());
        return -1;
      }
    }
    if (present != 0 && present != kKeysPerBatch) {
      std::fprintf(stderr, "iteration %d: batch %" PRIu64 " partially applied (%d of %d keys)\n",
                   iteration, b, present, kKeysPerBatch);
      return -1;
    }
    if (present == 0) {
      next = b;
      break;
    }
  }
  if (static_cast<int64_t>(next) <= max_acked) {
    std::fprintf(stderr,
                 "iteration %d: acknowledged batch %" PRId64 " lost (only %" PRIu64 " present)\n",
                 iteration, max_acked, next);
    return -1;
  }
  // Sequential writes: nothing past the first missing batch may exist.
  for (uint64_t b = next + 1; b < next + 50; ++b) {
    std::string value;
    if (db->Get({}, BatchKey(b, 0), &value).ok()) {
      std::fprintf(stderr, "iteration %d: batch %" PRIu64 " present after missing %" PRIu64 "\n",
                   iteration, b, next);
      return -1;
    }
  }
  std::string last;
  const Status ls = db->Get({}, "last_batch", &last);
  if (next > 0 && (!ls.ok() || last != std::to_string(next - 1))) {
    std::fprintf(stderr, "iteration %d: last_batch is %s, expected %" PRIu64 "\n", iteration,
                 ls.ok() ? last.c_str() : ls.ToString().c_str(), next - 1);
    return -1;
  }
  return static_cast<int64_t>(next);
}

int Run(int argc, char** argv) {
  int iterations = 100;
  uint64_t seed =
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::string dir;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--iterations") {
      iterations = std::atoi(argv[i + 1]);
    } else if (flag == "--seed") {
      seed = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (flag == "--dir") {
      dir = argv[i + 1];
    } else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 2;
    }
  }
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("talus_crash_" + std::to_string(getpid())))
              .string();
  }
  std::filesystem::remove_all(dir);
  std::printf("crash test: %d iterations, seed %" PRIu64 ", dir %s\n", iterations, seed,
              dir.c_str());
  std::fflush(stdout);

  std::mt19937_64 rng(seed);
  uint64_t next_batch = 0;
  uint64_t total_acked = 0;
  for (int it = 0; it < iterations; ++it) {
    int fds[2];
    if (pipe(fds) != 0) {
      std::perror("pipe");
      return 1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    if (pid == 0) {
      close(fds[0]);
      RunChild(dir, next_batch, fds[1]);
    }
    close(fds[1]);

    // Kill after 0-200 ms: sometimes during open/recovery, usually mid-write.
    const auto delay = std::chrono::milliseconds(rng() % 201);
    std::string partial;
    int64_t max_acked = ReadAcks(fds[0], std::chrono::steady_clock::now() + delay, &partial);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    // Acks written before the kill are still in the pipe: they count.
    max_acked = std::max(max_acked,
                         ReadAcks(fds[0], std::chrono::steady_clock::time_point::max(), &partial));
    close(fds[0]);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
      std::fprintf(stderr, "iteration %d: child exited on its own (status %d)\n", it, status);
      return 1;
    }

    const int64_t present = Verify(dir, next_batch, max_acked, it);
    if (present < 0) {
      std::fprintf(stderr, "FAILED (seed %" PRIu64 "); DB left in %s\n", seed, dir.c_str());
      return 1;
    }
    if (max_acked >= 0) {
      total_acked += static_cast<uint64_t>(max_acked + 1) - next_batch;
    }
    next_batch = static_cast<uint64_t>(present);
    if ((it + 1) % std::max(1, iterations / 10) == 0) {
      std::printf("  iteration %d: %" PRIu64 " batches in the DB\n", it + 1, next_batch);
      std::fflush(stdout);
    }
  }
  std::filesystem::remove_all(dir);
  std::printf("PASSED: %d kills, %" PRIu64 " acknowledged batches, %" PRIu64 " batches verified\n",
              iterations, total_acked, next_batch);
  return 0;
}

}  // namespace
}  // namespace talus

int main(int argc, char** argv) { return talus::Run(argc, argv); }
