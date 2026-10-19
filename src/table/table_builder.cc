#include "table/table_builder.h"

#include <cassert>

#include "db/dbformat.h"
#include "util/crc32c.h"
#include "util/file.h"
#include "util/hash.h"

namespace talus {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      // Index lookups binary-search restart points, so every index entry is
      // one: it costs little, because separators are short.
      index_block_(1) {
  if (!CompressionSupported(options.compression)) {
    status_ = Status::InvalidArgument("compression type not supported by this build");
  }
}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  assert(!finished_);
  if (!status_.ok()) {
    return;
  }
  assert(num_entries_ == 0 || InternalKeyComparator()(last_key_, internal_key) < 0);

  // Filter on user keys. Versions of one user key are adjacent, so comparing
  // with the previous key removes duplicates. last_key_ is still the full
  // previous key here: the separator below has not modified it yet.
  if (options_.filter_type != FilterType::kNone) {
    const std::string_view user_key = ExtractUserKey(internal_key);
    if (num_entries_ == 0 || ExtractUserKey(last_key_) != user_key) {
      key_hashes_.push_back(Hash64(user_key));
    }
  }
  if (pending_index_entry_) {
    InternalKeyComparator::FindShortestSeparator(&last_key_, internal_key);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }

  last_key_.assign(internal_key);
  ++num_entries_;
  data_block_.Add(internal_key, value);
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) {
    return;
  }
  WriteBlock(&data_block_, &pending_handle_);
  if (status_.ok()) {
    pending_index_entry_ = true;
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  const std::string_view raw = block->Finish();
  CompressionType type = options_.compression;
  std::string_view contents = raw;
  if (type != CompressionType::kNone) {
    CompressBlock(type, raw, &compressed_);
    // Keep the compressed form only if it saves at least 12.5%.
    if (compressed_.size() < raw.size() - raw.size() / 8) {
      contents = compressed_;
    } else {
      type = CompressionType::kNone;
    }
  }
  WriteRawBlock(contents, type, handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(std::string_view contents, CompressionType type,
                                 BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  status_ = file_->Append(contents);
  if (!status_.ok()) {
    return;
  }
  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(type);
  const uint32_t crc = crc32c::Extend(crc32c::Value(contents.data(), contents.size()), trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.ok()) {
    offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  assert(!finished_);
  FlushDataBlock();
  finished_ = true;
  if (!status_.ok()) {
    return status_;
  }

  Footer footer;
  if (options_.filter_type != FilterType::kNone) {
    std::string filter;
    BuildFilter(options_.filter_type, options_.bloom_bits_per_key, key_hashes_, &filter);
    // Filters are dense random bits; compressing them would not pay.
    WriteRawBlock(filter, CompressionType::kNone, &footer.filter);
  }
  if (status_.ok() && pending_index_entry_) {
    InternalKeyComparator::FindShortSuccessor(&last_key_);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }
  if (status_.ok()) {
    WriteBlock(&index_block_, &footer.index);
  }
  if (status_.ok()) {
    std::string encoded;
    footer.EncodeTo(&encoded);
    status_ = file_->Append(encoded);
    if (status_.ok()) {
      offset_ += encoded.size();
    }
  }
  return status_;
}

}  // namespace talus
