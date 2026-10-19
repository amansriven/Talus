#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Talus/status.h"
#include "util/file.h"

namespace talus::log {

// Reads records written by log::Writer.
class Reader {
 public:
  enum class Result {
    kRecord,      // A complete record whose checksum and type are valid.
    kEof,         // End of log. This includes a final record cut short by a
                  // crash mid-write: a missing header or payload tail.
    kCorruption,  // A complete record with a bad checksum or unknown type.
    kIOError,     // The file could not be read.
  };

  explicit Reader(std::unique_ptr<SequentialFile> file);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // On kRecord, *record holds the payload. It is backed by *scratch and stays
  // valid until the next call.
  Result ReadRecord(std::string_view* record, std::string* scratch);

  // Details of the last kCorruption or kIOError result.
  const Status& error() const { return error_; }

 private:
  std::unique_ptr<SequentialFile> file_;
  Status error_;
};

}  // namespace talus::log
