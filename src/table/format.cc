#include "table/format.h"

#include <cassert>
#include <limits>

#include "util/crc32c.h"
#include "util/file.h"

#if defined(TALUS_HAVE_LZ4)
#include <lz4.h>
#endif

namespace talus {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

bool BlockHandle::DecodeFrom(std::string_view* input) {
  return GetVarint64(input, &offset) && GetVarint64(input, &size);
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  filter.EncodeTo(dst);
  index.EncodeTo(dst);
  dst->resize(start + 2 * BlockHandle::kMaxEncodedLength);  // Zero padding.
  PutFixed64(dst, kTableMagicNumber);
  assert(dst->size() == start + kEncodedLength);
}

Status Footer::DecodeFrom(std::string_view input) {
  assert(input.size() == kEncodedLength);
  if (DecodeFixed64(input.data() + kEncodedLength - 8) != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  std::string_view handles = input.substr(0, 2 * BlockHandle::kMaxEncodedLength);
  if (!filter.DecodeFrom(&handles) || !index.DecodeFrom(&handles)) {
    return Status::Corruption("bad block handle in footer");
  }
  return Status::OK();
}

bool CompressionSupported(CompressionType type) {
  switch (type) {
    case CompressionType::kNone:
      return true;
    case CompressionType::kLZ4:
#if defined(TALUS_HAVE_LZ4)
      return true;
#else
      return false;
#endif
  }
  return false;
}

// LZ4 block payload: varint32 uncompressed_size | LZ4 block-format data.
void CompressBlock(CompressionType type, std::string_view raw, std::string* out) {
  assert(type == CompressionType::kLZ4 && CompressionSupported(type));
  out->clear();
#if defined(TALUS_HAVE_LZ4)
  PutVarint32(out, static_cast<uint32_t>(raw.size()));
  const size_t header = out->size();
  const int bound = LZ4_compressBound(static_cast<int>(raw.size()));
  out->resize(header + static_cast<size_t>(bound));
  const int n =
      LZ4_compress_default(raw.data(), out->data() + header, static_cast<int>(raw.size()), bound);
  out->resize(header + static_cast<size_t>(n));
#else
  (void)type;
  (void)raw;
#endif
}

namespace {

Status DecompressLZ4(std::string_view payload, BlockContents* result) {
#if defined(TALUS_HAVE_LZ4)
  uint32_t raw_size = 0;
  if (!GetVarint32(&payload, &raw_size)) {
    return Status::Corruption("bad LZ4 block header");
  }
  // LZ4 expands at most ~255x. Bounding the claimed size first means a
  // corrupt header cannot trigger a huge allocation.
  if (raw_size > payload.size() * 255 + 16 || payload.size() > std::numeric_limits<int>::max()) {
    return Status::Corruption("bad LZ4 block size");
  }
  auto buf = std::make_unique_for_overwrite<char[]>(raw_size);
  const int n = LZ4_decompress_safe(payload.data(), buf.get(), static_cast<int>(payload.size()),
                                    static_cast<int>(raw_size));
  if (n < 0 || static_cast<uint32_t>(n) != raw_size) {
    return Status::Corruption("corrupt LZ4 block");
  }
  result->data = std::string_view(buf.get(), raw_size);
  result->heap = std::move(buf);
  return Status::OK();
#else
  (void)payload;
  (void)result;
  return Status::Corruption("block is LZ4-compressed but LZ4 support is not built in");
#endif
}

}  // namespace

Status ReadBlock(const RandomAccessFile& file, const BlockHandle& handle, BlockContents* result) {
  if (handle.offset > file.size() || handle.size > file.size() ||
      handle.size + kBlockTrailerSize > file.size() - handle.offset) {
    return Status::Corruption("block handle beyond end of file");
  }
  const auto n = static_cast<size_t>(handle.size);
  std::unique_ptr<char[]> buf;
  if (!file.is_mmap()) {
    buf = std::make_unique_for_overwrite<char[]>(n + kBlockTrailerSize);
  }
  std::string_view contents;
  Status s = file.Read(handle.offset, n + kBlockTrailerSize, &contents, buf.get());
  if (!s.ok()) {
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  const char* data = contents.data();
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(data + n + 1));
  if (crc32c::Value(data, n + 1) != expected) {  // Covers the type byte too.
    return Status::Corruption("block checksum mismatch");
  }
  switch (static_cast<CompressionType>(data[n])) {
    case CompressionType::kNone:
      result->data = std::string_view(data, n);
      result->heap = std::move(buf);
      return Status::OK();
    case CompressionType::kLZ4:
      return DecompressLZ4(std::string_view(data, n), result);
  }
  return Status::Corruption("unknown block compression type");
}

}  // namespace talus
