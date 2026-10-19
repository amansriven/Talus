// Fuzzes the block reader: arbitrary bytes as a block, then a full scan and
// several seeks. The reader must never crash or read outside the block,
// which ASan checks.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "db/dbformat.h"
#include "table/block.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace talus;
  // An exactly-sized heap copy, so any overread is caught.
  Buffer heap = AllocateBuffer(size, nullptr);
  if (size > 0) {
    std::memcpy(heap.get(), data, size);
  }
  BlockContents contents;
  contents.data = std::string_view(heap.get(), size);
  contents.heap = std::move(heap);
  const Block block(std::move(contents));

  BlockIter<> iter(block);
  size_t steps = 0;
  for (iter.SeekToFirst(); iter.Valid() && steps < 10000; iter.Next(), ++steps) {
    volatile size_t sink = iter.key().size() + iter.value().size();
    (void)sink;
  }
  // Seek to internal keys built from the input's own bytes.
  for (size_t i = 0; i + 1 < size && i < 64; i += 16) {
    std::string target(reinterpret_cast<const char*>(data) + i, std::min<size_t>(8, size - i));
    AppendInternalKey(&target, "", 0, ValueType::kValue);
    iter.Seek(target);
  }
  return 0;
}
