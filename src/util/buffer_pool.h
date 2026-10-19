#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace talus {

// Supplies block buffers from 2 MiB chunks advised with MADV_HUGEPAGE, so
// cached blocks sit on huge pages (fewer TLB misses). Used for block-cache
// memory when Options::use_huge_pages is set. Requests are rounded up to
// 512-byte size classes up to 64 KiB, and freed buffers go on a per-class free
// list for reuse. Larger requests go to the regular heap. Chunks return to
// the OS only when the pool is destroyed. Thread-safe.
//
// On platforms without MADV_HUGEPAGE (macOS) the chunks are ordinary memory,
// so the pool still works but gives no TLB benefit.
class HugePageBufferPool {
 public:
  static constexpr size_t kGranularity = 512;
  static constexpr size_t kMaxPooledSize = 64 * 1024;

  HugePageBufferPool() = default;
  // REQUIRES: every buffer has been freed.
  ~HugePageBufferPool();

  HugePageBufferPool(const HugePageBufferPool&) = delete;
  HugePageBufferPool& operator=(const HugePageBufferPool&) = delete;

  char* Allocate(size_t n);
  // `n` must be the size passed to Allocate().
  void Free(char* p, size_t n);

  // Bytes obtained from the system for pooled chunks.
  size_t ChunkBytes() const;

 private:
  static constexpr size_t kNumClasses = kMaxPooledSize / kGranularity;

  struct FreeNode {
    FreeNode* next;
  };

  static size_t ClassOf(size_t n) { return (n + kGranularity - 1) / kGranularity - 1; }

  mutable std::mutex mu_;
  FreeNode* free_lists_[kNumClasses] = {};  // Guarded by mu_.
  std::vector<void*> chunks_;               // Guarded by mu_.
  char* cursor_ = nullptr;                  // Unused tail of the newest chunk.
  size_t remaining_ = 0;
};

// Frees a buffer from AllocateBuffer(): back to its pool, or to the heap.
struct BufferDeleter {
  HugePageBufferPool* pool = nullptr;
  size_t size = 0;
  void operator()(char* p) const;
};

// An owned byte buffer, from a pool or from the heap.
using Buffer = std::unique_ptr<char[], BufferDeleter>;

// Allocates `n` bytes from `pool`, or from the heap if `pool` is null.
Buffer AllocateBuffer(size_t n, HugePageBufferPool* pool);

}  // namespace talus
