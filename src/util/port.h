#pragma once

// Small platform abstractions shared across the engine.

#include <cstddef>

namespace talus {

// The granularity at which concurrently written data must be separated to
// avoid false sharing. Apple Silicon uses 128-byte cache lines; x86-64 and
// most other ARM64 cores use 64-byte lines.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr size_t kCacheLineSize = 128;
#else
inline constexpr size_t kCacheLineSize = 64;
#endif

// Huge-page size targeted by MADV_HUGEPAGE (transparent huge pages, x86-64 and
// the common ARM64 Linux configuration).
inline constexpr size_t kHugePageSize = size_t{2} << 20;

}  // namespace talus

#if defined(__GNUC__) || defined(__clang__)
#define TALUS_LIKELY(x) (__builtin_expect(!!(x), 1))
#define TALUS_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define TALUS_LIKELY(x) (x)
#define TALUS_UNLIKELY(x) (x)
#endif
