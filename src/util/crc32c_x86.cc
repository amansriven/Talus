// SSE4.2 hardware CRC-32C for x86-64. The function is compiled with a target
// attribute instead of a file-wide -msse4.2, so the rest of the binary stays
// runnable on CPUs without SSE4.2 and dispatch happens at runtime.

#include "util/crc32c.h"

#if defined(__x86_64__)

#include <nmmintrin.h>

#include <cstdint>
#include <cstring>

namespace talus::crc32c::internal {
namespace {

__attribute__((target("sse4.2"))) uint32_t ExtendSse42(uint32_t crc, const char* data,
                                                       size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint64_t c = crc ^ 0xffffffffu;

  // Align to 8 bytes so the 8-byte loads below never straddle a cache line.
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 7u) != 0) {
    c = _mm_crc32_u8(static_cast<uint32_t>(c), *p);
    ++p;
    --n;
  }
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, sizeof(word));
    c = _mm_crc32_u64(c, word);
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t word;
    std::memcpy(&word, p, sizeof(word));
    c = _mm_crc32_u32(static_cast<uint32_t>(c), word);
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    c = _mm_crc32_u8(static_cast<uint32_t>(c), *p);
    ++p;
    --n;
  }
  return static_cast<uint32_t>(c) ^ 0xffffffffu;
}

}  // namespace

ExtendFn X86Sse42Extend() {
  static const bool kSupported = __builtin_cpu_supports("sse4.2") != 0;
  return kSupported ? &ExtendSse42 : nullptr;
}

}  // namespace talus::crc32c::internal

#endif  // defined(__x86_64__)
