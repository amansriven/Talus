// ARMv8 CRC-extension CRC-32C for ARM64. On Linux this file is compiled with
// -march=armv8-a+crc (see src/CMakeLists.txt) and the instructions are only
// used after an AT_HWCAP check. Every Apple Silicon CPU implements them.

#include "util/crc32c.h"

#if defined(__aarch64__)

#include <cstdint>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace talus::crc32c::internal {
namespace {

#if defined(__ARM_FEATURE_CRC32)

uint32_t ExtendArm64(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t c = crc ^ 0xffffffffu;

  // Align to 8 bytes so the 8-byte loads below never straddle a cache line.
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 7u) != 0) {
    c = __crc32cb(c, *p);
    ++p;
    --n;
  }
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, sizeof(word));
    c = __crc32cd(c, word);
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t word;
    std::memcpy(&word, p, sizeof(word));
    c = __crc32cw(c, word);
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    c = __crc32cb(c, *p);
    ++p;
    --n;
  }
  return c ^ 0xffffffffu;
}

bool CpuHasCrc() {
#if defined(__APPLE__)
  return true;
#elif defined(__linux__)
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
  return false;
#endif
}

#endif  // defined(__ARM_FEATURE_CRC32)

}  // namespace

ExtendFn Arm64CrcExtend() {
#if defined(__ARM_FEATURE_CRC32)
  static const bool kSupported = CpuHasCrc();
  return kSupported ? &ExtendArm64 : nullptr;
#else
  return nullptr;
#endif
}

}  // namespace talus::crc32c::internal

#endif  // defined(__aarch64__)
