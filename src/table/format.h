#pragma once

// SSTable file layout:
//
//   [data block 0] ... [data block N] [filter block] [index block] [footer]
//
// Every block is followed by a 5-byte trailer:
//   compression_type (1B) | masked crc32c (fixed32) over block bytes + type
// The footer is the last 48 bytes of the file:
//   filter BlockHandle | index BlockHandle | zero padding to 40 B | magic (fixed64)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Talus/options.h"
#include "Talus/status.h"
#include "util/buffer_pool.h"
#include "util/coding.h"

namespace talus {

class RandomAccessFile;

// The bytes "TalusSST" read as a little-endian fixed64.
inline constexpr uint64_t kTableMagicNumber = 0x54535373756C6154ULL;

inline constexpr size_t kBlockTrailerSize = 5;

// The location of a block within a file (excluding its trailer).
struct BlockHandle {
  static constexpr size_t kMaxEncodedLength = 2 * kMaxVarint64Bytes;

  uint64_t offset = 0;
  uint64_t size = 0;

  // varint64 offset | varint64 size
  void EncodeTo(std::string* dst) const;
  bool DecodeFrom(std::string_view* input);
};

struct Footer {
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;
  static_assert(kEncodedLength == 48);

  BlockHandle filter;  // size 0 if the table has no filter.
  BlockHandle index;

  // Appends exactly kEncodedLength bytes.
  void EncodeTo(std::string* dst) const;
  // `input` must be exactly kEncodedLength bytes.
  Status DecodeFrom(std::string_view input);
};

// A block's bytes after verification and decompression.
struct BlockContents {
  std::string_view data;
  // Owns `data`, unless it points into a memory-mapped file (then null).
  Buffer heap;
};

// Reads the block at `handle`, always verifies its checksum, and
// decompresses it if needed. Buffers come from `pool` if it is non-null.
// Returns Corruption on a bad checksum, an unknown compression type, or a
// handle beyond the end of the file.
Status ReadBlock(const RandomAccessFile& file, const BlockHandle& handle, HugePageBufferPool* pool,
                 BlockContents* result);

// True if this build can write and read blocks compressed with `type`.
bool CompressionSupported(CompressionType type);

// Replaces *out with the compressed form of `raw`. REQUIRES:
// CompressionSupported(type) and type != kNone.
void CompressBlock(CompressionType type, std::string_view raw, std::string* out);

}  // namespace talus
