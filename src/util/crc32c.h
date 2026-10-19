#pragma once

// CRC-32C (Castagnoli polynomial 0x1EDC6F41), the checksum used for WAL and
// MANIFEST records and SSTable block trailers.
//
// Extend() dispatches at runtime to the fastest implementation the CPU
// supports: SSE4.2 CRC32 on x86-64, the ARMv8 CRC extension on ARM64, or a
// table-driven slicing-by-8 software fallback.

#include <cstddef>
#include <cstdint>

namespace talus::crc32c {

// Returns the CRC-32C of concat(A, data[0, n)), where `crc` is the CRC-32C of
// some byte string A. Extend(0, data, n) is the CRC-32C of data alone.
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// CRCs stored on disk are masked. Computing the CRC of a string that contains
// embedded CRCs is otherwise prone to degenerate cases, so a stored CRC is
// rotated and offset by a constant (the same transform LevelDB uses).
inline constexpr uint32_t kMaskDelta = 0xa282ead8u;

inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }

inline uint32_t Unmask(uint32_t masked) {
  const uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

// True if Extend() uses a hardware CRC instruction on this machine.
bool IsHardwareAccelerated();

// "sse4.2", "armv8-crc" or "software".
const char* ImplementationName();

namespace internal {

using ExtendFn = uint32_t (*)(uint32_t crc, const char* data, size_t n);

// The portable slicing-by-8 implementation.
uint32_t ExtendSoftware(uint32_t crc, const char* data, size_t n);

// The hardware implementation for this CPU, or nullptr if the architecture or
// CPU lacks CRC-32C instructions. Exposed so tests and benchmarks can exercise
// every path explicitly.
ExtendFn HardwareExtend();

// Per-architecture probes, each defined only when compiled for that
// architecture. They return nullptr when the running CPU lacks support.
ExtendFn X86Sse42Extend();
ExtendFn Arm64CrcExtend();

}  // namespace internal
}  // namespace talus::crc32c
