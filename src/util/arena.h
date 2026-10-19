#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "util/port.h"

namespace talus {

// A thread-safe bump allocator backing memtable nodes.
//
// Memory is carved out of fixed-size blocks and is never freed individually;
// everything is released when the Arena is destroyed. Allocate() is lock-free
// in the common case: it claims a range of the current block with a single
// atomic fetch_add. Only installing a new block takes a mutex.
//
// Invariants:
//  * `current_` always points to a fully constructed block. A new block is
//    initialized (including claiming the requesting thread's first range)
//    before being published with a release store, and readers load it with
//    acquire, so they never observe a partially constructed header.
//  * A block's `used` counter only grows. Each fetch_add returns a distinct
//    starting offset, so the ranges [off, off + bytes) handed out within one
//    block never overlap. When the counter passes `capacity`, the thread that
//    crossed it falls through to the slow path; the block's unused tail is
//    abandoned rather than reused.
//  * Blocks are freed only in the destructor, so a pointer returned by
//    Allocate() stays valid for the lifetime of the Arena with no further
//    reclamation protocol.
//
// Block memory comes from std::aligned_alloc (not operator new), so arena
// growth is invisible to the allocation-counting test, which deliberately
// excludes it.
class Arena {
 public:
  static constexpr size_t kDefaultBlockSize = 4096;

  // Every returned pointer is aligned to this, which suffices for
  // std::atomic<T*> and all integer types stored in arena memory. (Using
  // alignof(max_align_t), 16 on x86-64, would waste space in every node.)
  static constexpr size_t kAlignment = 8;
  static_assert(alignof(std::atomic<void*>) <= kAlignment);
  static_assert(alignof(uint64_t) <= kAlignment);

  // With `use_huge_pages`, blocks are 2 MiB, 2 MiB-aligned and advised with
  // MADV_HUGEPAGE on Linux. Elsewhere the flag only changes the block size.
  explicit Arena(size_t block_size = kDefaultBlockSize, bool use_huge_pages = false);
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns a pointer to `bytes` (> 0) bytes of uninitialized memory aligned to
  // kAlignment. Safe to call concurrently from any number of threads.
  char* Allocate(size_t bytes);

  // Bytes obtained from the system so far (block headers and abandoned tails
  // included). Approximate while other threads are allocating.
  size_t MemoryUsage() const {
    // Relaxed: a statistic used to decide when to switch memtables; it need
    // not be ordered with the allocations it describes.
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  struct Block;

  char* AllocateSlow(size_t bytes, Block* observed);
  char* AllocateDedicated(size_t bytes);
  Block* NewBlockLocked(size_t data_bytes, bool huge);

  const size_t block_size_;
  const bool use_huge_pages_;

  std::atomic<Block*> current_;
  std::atomic<size_t> memory_usage_{0};

  std::mutex mu_;
  Block* blocks_ = nullptr;  // Singly linked list of every block; guarded by mu_.
};

}  // namespace talus
