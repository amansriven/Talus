#include "util/arena.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace talus {

// The header occupies its own cache line(s): `used` is the one word every
// allocating thread writes, and keeping it off the first data line means
// threads initializing freshly allocated nodes do not invalidate it.
struct alignas(kCacheLineSize) Arena::Block {
  Block* next = nullptr;     // Guarded by Arena::mu_.
  size_t capacity = 0;       // Usable bytes in data(); immutable once published.
  size_t total_bytes = 0;    // Bytes obtained from the system, header included.
  std::atomic<size_t> used{0};

  char* data() { return reinterpret_cast<char*>(this) + sizeof(Block); }
};

namespace {

constexpr size_t RoundUp(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

}  // namespace

Arena::Arena(size_t block_size, bool use_huge_pages)
    : block_size_(use_huge_pages ? kHugePageSize
                                 : RoundUp(std::max(block_size, 4 * sizeof(Block)), kCacheLineSize)),
      use_huge_pages_(use_huge_pages) {
  std::lock_guard<std::mutex> lock(mu_);
  Block* first = NewBlockLocked(block_size_ - sizeof(Block), use_huge_pages_);
  // Relaxed: the constructor happens-before any use of the Arena by other
  // threads (publishing the Arena itself provides that ordering).
  current_.store(first, std::memory_order_relaxed);
}

Arena::~Arena() {
  Block* b = blocks_;
  while (b != nullptr) {
    Block* next = b->next;
    b->~Block();
    std::free(b);  // NOLINT(cppcoreguidelines-no-malloc): paired with aligned_alloc.
    b = next;
  }
}

char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  bytes = RoundUp(bytes, kAlignment);
  // Large requests get their own block so they do not waste the tail of the
  // shared one. Memtable entries are almost always far below this.
  if (TALUS_UNLIKELY(bytes > (block_size_ - sizeof(Block)) / 4)) {
    return AllocateDedicated(bytes);
  }

  // Acquire pairs with the release store in AllocateSlow(), so the block's
  // `capacity` and `used` initialization are visible before we use them.
  Block* block = current_.load(std::memory_order_acquire);
  // Relaxed: atomicity alone guarantees each caller a distinct range (all
  // RMWs on `used` form one total modification order). The memory handed out
  // carries no data yet; callers publish what they write into it with their
  // own release operations (e.g. the skiplist's CAS on a next pointer).
  const size_t offset = block->used.fetch_add(bytes, std::memory_order_relaxed);
  if (TALUS_LIKELY(offset + bytes <= block->capacity)) {
    return block->data() + offset;
  }
  return AllocateSlow(bytes, block);
}

char* Arena::AllocateSlow(size_t bytes, Block* observed) {
  std::lock_guard<std::mutex> lock(mu_);
  // Relaxed: `current_` is only stored while holding mu_, which we hold.
  Block* current = current_.load(std::memory_order_relaxed);
  if (current != observed) {
    // Another thread installed a fresh block while we waited for the lock.
    const size_t offset = current->used.fetch_add(bytes, std::memory_order_relaxed);
    if (offset + bytes <= current->capacity) {
      return current->data() + offset;
    }
  }
  Block* fresh = NewBlockLocked(block_size_ - sizeof(Block), use_huge_pages_);
  // Claim our range before publishing, so no other thread can be handed it.
  fresh->used.store(bytes, std::memory_order_relaxed);
  // Release: publishes the fully initialized block header (see Allocate()).
  current_.store(fresh, std::memory_order_release);
  return fresh->data();
}

char* Arena::AllocateDedicated(size_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  Block* block = NewBlockLocked(bytes, /*huge=*/false);
  // Relaxed: this block is never published as current_; only we use it.
  block->used.store(bytes, std::memory_order_relaxed);
  return block->data();
}

Arena::Block* Arena::NewBlockLocked(size_t data_bytes, bool huge) {
  const size_t align = huge ? kHugePageSize : kCacheLineSize;
  const size_t total = RoundUp(sizeof(Block) + data_bytes, align);
  void* mem = std::aligned_alloc(align, total);  // NOLINT(cppcoreguidelines-no-malloc)
  if (mem == nullptr) {
    std::fprintf(stderr, "talus: arena allocation of %zu bytes failed\n", total);
    std::abort();
  }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  if (huge) {
    // Advisory only: if transparent huge pages are disabled the kernel ignores
    // it and we fall back to base pages, so the result is deliberately unused.
    (void)madvise(mem, total, MADV_HUGEPAGE);
  }
#endif
  auto* block = new (mem) Block;
  block->capacity = total - sizeof(Block);
  block->total_bytes = total;
  block->next = blocks_;
  blocks_ = block;
  // Relaxed: a statistic (see MemoryUsage()).
  memory_usage_.fetch_add(total, std::memory_order_relaxed);
  return block;
}

}  // namespace talus
