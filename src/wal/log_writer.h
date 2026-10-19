#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "Talus/status.h"
#include "util/file.h"

namespace talus::log {

// Appends records to a log file. Not thread-safe: the write group's leader
// is the only writer.
class Writer {
 public:
  explicit Writer(std::unique_ptr<WritableFile> dest);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Appends one record and hands it to the OS, so it survives a process
  // crash. Call Sync() to make it survive a machine crash too.
  Status AddRecord(std::string_view payload);
  Status Sync() { return dest_->Sync(); }

  WritableFile* file() { return dest_.get(); }

 private:
  std::unique_ptr<WritableFile> dest_;
  const uint32_t type_crc_;  // CRC-32C of the type byte, precomputed.
};

}  // namespace talus::log
