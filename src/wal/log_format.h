#pragma once

// Record format shared by WAL (NNNNNN.log) and MANIFEST files:
//
//   masked_crc32c (fixed32) | length (fixed32) | type (1B) | payload[length]
//
// The CRC covers the type byte and the payload. Records are not fragmented.

#include <cstddef>
#include <cstdint>

namespace talus::log {

enum RecordType : uint8_t {
  // 0 is reserved, so zero-filled space is never mistaken for a record.
  kFullType = 1,
};

inline constexpr size_t kHeaderSize = 4 + 4 + 1;

}  // namespace talus::log
