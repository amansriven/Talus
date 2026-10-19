#include "util/crc32c.h"

#include <array>
#include <cstring>

namespace talus::crc32c {
namespace {

// The Castagnoli polynomial in reflected (LSB-first) bit order.
constexpr uint32_t kReflectedPoly = 0x82f63b78u;

using Table = std::array<std::array<uint32_t, 256>, 8>;

// kTables[k][b] is the CRC contribution of byte b followed by k zero bytes.
// Slicing-by-8 uses all eight tables to fold 8 input bytes per iteration.
constexpr Table MakeTables() {
  Table t{};
  for (uint32_t b = 0; b < 256; ++b) {
    uint32_t crc = b;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0 ? (crc >> 1) ^ kReflectedPoly : crc >> 1;
    }
    t[0][b] = crc;
  }
  for (size_t k = 1; k < t.size(); ++k) {
    for (uint32_t b = 0; b < 256; ++b) {
      t[k][b] = (t[k - 1][b] >> 8) ^ t[0][t[k - 1][b] & 0xffu];
    }
  }
  return t;
}

constexpr Table kTables = MakeTables();

internal::ExtendFn ChooseImplementation() {
  internal::ExtendFn hw = internal::HardwareExtend();
  return hw != nullptr ? hw : &internal::ExtendSoftware;
}

}  // namespace

namespace internal {

uint32_t ExtendSoftware(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t c = crc ^ 0xffffffffu;
  while (n >= 8) {
    uint32_t lo;
    uint32_t hi;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    lo ^= c;
    // The first input byte still has 7 bytes to pass through, hence table 7.
    c = kTables[7][lo & 0xffu] ^ kTables[6][(lo >> 8) & 0xffu] ^ kTables[5][(lo >> 16) & 0xffu] ^
        kTables[4][lo >> 24] ^ kTables[3][hi & 0xffu] ^ kTables[2][(hi >> 8) & 0xffu] ^
        kTables[1][(hi >> 16) & 0xffu] ^ kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    c = kTables[0][(c ^ *p) & 0xffu] ^ (c >> 8);
    ++p;
    --n;
  }
  return c ^ 0xffffffffu;
}

ExtendFn HardwareExtend() {
#if defined(__x86_64__) || defined(_M_X64)
  return X86Sse42Extend();
#elif defined(__aarch64__) || defined(_M_ARM64)
  return Arm64CrcExtend();
#else
  return nullptr;
#endif
}

}  // namespace internal

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  // Resolved once on first use. A function-local static (rather than a
  // namespace-scope one) keeps this safe to call from other static
  // initializers.
  static const internal::ExtendFn kImpl = ChooseImplementation();
  return kImpl(crc, data, n);
}

bool IsHardwareAccelerated() { return internal::HardwareExtend() != nullptr; }

const char* ImplementationName() {
  if (!IsHardwareAccelerated()) {
    return "software";
  }
#if defined(__x86_64__) || defined(_M_X64)
  return "sse4.2";
#else
  return "armv8-crc";
#endif
}

}  // namespace talus::crc32c
