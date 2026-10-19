#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Talus/options.h"
#include "Talus/status.h"
#include "table/block_builder.h"
#include "table/filter_policy.h"
#include "table/format.h"

namespace talus {

class WritableFile;

struct TableOptions {
  size_t block_size = 4096;
  int block_restart_interval = 16;
  FilterType filter_type = FilterType::kBlockedBloom;
  int bloom_bits_per_key = 10;
  CompressionType compression = CompressionType::kNone;
};

// Writes an SSTable (see table/format.h) of internal keys to a file.
class TableBuilder {
 public:
  // `file` must outlive the builder. The caller syncs and closes it after
  // Finish().
  TableBuilder(const TableOptions& options, WritableFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: `internal_key` is greater than every key added before;
  // Finish() not called.
  void Add(std::string_view internal_key, std::string_view value);

  // Writes the remaining data block, the filter, the index and the footer.
  Status Finish();

  // OK unless a write failed, or the options are unsupported.
  const Status& status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  // Bytes written so far (the final file size after Finish()).
  uint64_t FileSize() const { return offset_; }

 private:
  void FlushDataBlock();
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(std::string_view contents, CompressionType type, BlockHandle* handle);

  const TableOptions options_;
  WritableFile* const file_;
  Status status_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  bool finished_ = false;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  // After a data block is written, its index entry waits for the next key,
  // so the separator can be shortened against it.
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;

  std::vector<uint64_t> key_hashes_;  // Hash64 of each distinct user key.
  std::string compressed_;            // Reused compression output buffer.
};

}  // namespace talus
