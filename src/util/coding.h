#pragma once

// Fixed-width and variable-length integer encodings used by every on-disk
// format (WAL records, blocks, footers, manifest edits).
//
// Fixed-width integers are little-endian. Varints use the LEB128-style
// encoding of LevelDB/protobuf: 7 data bits per byte, least significant group
// first, high bit set on every byte except the last.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace talus {

static_assert(std::endian::native == std::endian::little,
              "Talus encodes integers with memcpy and supports little-endian hosts only");

inline constexpr int kMaxVarint32Bytes = 5;
inline constexpr int kMaxVarint64Bytes = 10;

inline void EncodeFixed32(char* dst, uint32_t value) { std::memcpy(dst, &value, sizeof(value)); }
inline void EncodeFixed64(char* dst, uint64_t value) { std::memcpy(dst, &value, sizeof(value)); }

inline uint32_t DecodeFixed32(const char* src) {
  uint32_t value;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

inline uint64_t DecodeFixed64(const char* src) {
  uint64_t value;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

// Returns the number of bytes the varint encoding of `value` occupies.
inline int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++len;
  }
  return len;
}

// Writes the varint encoding of `value` at `dst`, which must have room for
// kMaxVarint32Bytes / kMaxVarint64Bytes. Returns the position after the last
// byte written.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

// Appends varint32(value.size()) followed by the bytes of `value`.
void PutLengthPrefixed(std::string* dst, std::string_view value);

// Decodes a varint from [p, limit). Returns the position after the varint, or
// nullptr if the input is truncated, longer than the maximum encoding, or
// encodes a value that does not fit in the destination type.
const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  // Fast path: most lengths in blocks and WAL records fit in one byte.
  if (p < limit) {
    const auto byte = static_cast<uint8_t>(*p);
    if (byte < 0x80) {
      *value = byte;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

// The following parse a value from the front of `*input` and advance it past
// the consumed bytes. On failure they return false and leave `*input` in an
// unspecified state.
bool GetFixed32(std::string_view* input, uint32_t* value);
bool GetFixed64(std::string_view* input, uint64_t* value);
bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

}  // namespace talus
