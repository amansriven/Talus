#pragma once

#include <cstdint>

namespace talus {

// How SSTable blocks are compressed. The value is stored in each block
// trailer, so it is part of the file format.
enum class CompressionType : uint8_t {
  kNone = 0,
  kLZ4 = 1,  // Only if built with -DTalus_WITH_LZ4=ON.
};

struct WriteOptions {
  // If true, the write is durable (its WAL record fsynced) before Write()
  // returns. If false, a process crash cannot lose it, but a machine crash
  // can lose the most recent writes.
  bool sync = false;
};

}  // namespace talus
