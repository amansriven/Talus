#include "util/buffer_pool.h"

#include <cstdio>
#include <cstdlib>

#include "util/port.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace talus {

HugePageBufferPool::~HugePageBufferPool() {
  for (void* chunk : chunks_) {
    std::free(chunk);  // NOLINT(cppcoreguidelines-no-malloc): paired with aligned_alloc.
  }
}

char* HugePageBufferPool::Allocate(size_t n) {
  if (n == 0 || n > kMaxPooledSize) {
    return new char[n];  // This file is the pool implementation (spec §1).
  }
  const size_t cls = ClassOf(n);
  const size_t class_size = (cls + 1) * kGranularity;
  std::lock_guard<std::mutex> lock(mu_);
  if (FreeNode* node = free_lists_[cls]) {
    free_lists_[cls] = node->next;
    return reinterpret_cast<char*>(node);
  }
  if (remaining_ < class_size) {
    // The rest of the current chunk is abandoned: it is under 64 KiB, about
    // 3% of a chunk at worst.
    void* chunk =
        std::aligned_alloc(kHugePageSize, kHugePageSize);  // NOLINT(cppcoreguidelines-no-malloc)
    if (chunk == nullptr) {
      std::fprintf(stderr, "talus: buffer pool allocation failed\n");
      std::abort();
    }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    (void)madvise(chunk, kHugePageSize, MADV_HUGEPAGE);  // Advisory; failure is harmless.
#endif
    chunks_.push_back(chunk);
    cursor_ = static_cast<char*>(chunk);
    remaining_ = kHugePageSize;
  }
  char* p = cursor_;
  cursor_ += class_size;
  remaining_ -= class_size;
  return p;
}

void HugePageBufferPool::Free(char* p, size_t n) {
  if (n == 0 || n > kMaxPooledSize) {
    delete[] p;
    return;
  }
  auto* node = reinterpret_cast<FreeNode*>(p);
  std::lock_guard<std::mutex> lock(mu_);
  node->next = free_lists_[ClassOf(n)];
  free_lists_[ClassOf(n)] = node;
}

size_t HugePageBufferPool::ChunkBytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return chunks_.size() * kHugePageSize;
}

void BufferDeleter::operator()(char* p) const {
  if (pool != nullptr) {
    pool->Free(p, size);
  } else {
    delete[] p;
  }
}

Buffer AllocateBuffer(size_t n, HugePageBufferPool* pool) {
  char* p = pool != nullptr ? pool->Allocate(n) : new char[n];
  return Buffer(p, BufferDeleter{pool, n});
}

}  // namespace talus
