#include "util/coding.h"

namespace talus {

char* EncodeVarint32(char* dst, uint32_t value) {
  return EncodeVarint64(dst, value);
}

char* EncodeVarint64(char* dst, uint64_t value) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  while (value >= 0x80) {
    *ptr++ = static_cast<uint8_t>(value | 0x80);
    value >>= 7;
  }
  *ptr++ = static_cast<uint8_t>(value);
  return reinterpret_cast<char*>(ptr);
}

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutVarint32(std::string* dst, uint32_t value) {
  char buf[kMaxVarint32Bytes];
  char* end = EncodeVarint32(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t value) {
  char buf[kMaxVarint64Bytes];
  char* end = EncodeVarint64(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (int i = 0; i < kMaxVarint32Bytes && p < limit; ++i) {
    const auto byte = static_cast<uint8_t>(*p++);
    // The fifth byte carries bits 28..31; anything above that would overflow.
    if (i == kMaxVarint32Bytes - 1 && byte > 0x0f) {
      return nullptr;
    }
    result |= static_cast<uint32_t>(byte & 0x7f) << (7 * i);
    if (byte < 0x80) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (int i = 0; i < kMaxVarint64Bytes && p < limit; ++i) {
    const auto byte = static_cast<uint8_t>(*p++);
    // The tenth byte carries bit 63 only.
    if (i == kMaxVarint64Bytes - 1 && byte > 0x01) {
      return nullptr;
    }
    result |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
    if (byte < 0x80) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetFixed32(std::string_view* input, uint32_t* value) {
  if (input->size() < sizeof(uint32_t)) {
    return false;
  }
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed64(std::string_view* input, uint64_t* value) {
  if (input->size() < sizeof(uint64_t)) {
    return false;
  }
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
  const char* begin = input->data();
  const char* end = GetVarint32Ptr(begin, begin + input->size(), value);
  if (end == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(end - begin));
  return true;
}

bool GetVarint64(std::string_view* input, uint64_t* value) {
  const char* begin = input->data();
  const char* end = GetVarint64Ptr(begin, begin + input->size(), value);
  if (end == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(end - begin));
  return true;
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len;
  if (!GetVarint32(input, &len) || input->size() < len) {
    return false;
  }
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace talus
