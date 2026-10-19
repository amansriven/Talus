#pragma once

#include <atomic>
#include <cstdint>

namespace talus {

// A fast per-thread pseudo-random generator (xorshift64*), used for skiplist
// node heights. Not cryptographically secure. Each thread's state is seeded
// once, from a global counter mixed with SplitMix64, so threads get
// independent nonzero streams and the hot path touches only thread-local
// state.
inline uint64_t ThreadLocalRandom() {
  thread_local uint64_t state = [] {
    static std::atomic<uint64_t> seed_counter{0};
    // Relaxed: the counter only needs to hand out distinct values.
    uint64_t z = seed_counter.fetch_add(1, std::memory_order_relaxed) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return z != 0 ? z : 1;  // xorshift's all-zero state is a fixed point.
  }();
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545F4914F6CDD1DULL;
}

}  // namespace talus
