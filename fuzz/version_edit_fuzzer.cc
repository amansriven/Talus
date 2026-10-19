// Fuzzes the VersionEdit (MANIFEST record) decoder. Decoding must never
// crash, and anything it accepts must round-trip: re-encoding and decoding
// again yields the same encoding.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "version/version_edit.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace talus;
  VersionEdit edit;
  if (!edit.DecodeFrom(std::string_view(reinterpret_cast<const char*>(data), size)).ok()) {
    return 0;
  }
  std::string encoded;
  edit.EncodeTo(&encoded);
  VersionEdit again;
  if (!again.DecodeFrom(encoded).ok()) {
    std::abort();
  }
  std::string reencoded;
  again.EncodeTo(&reencoded);
  if (reencoded != encoded) {
    std::abort();
  }
  return 0;
}
