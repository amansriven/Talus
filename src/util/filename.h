#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Talus/status.h"

namespace talus {

// Files in a database directory. Numbers are zero-padded to at least six
// digits.
enum class FileType {
  kLogFile,         // NNNNNN.log      write-ahead log
  kTableFile,       // NNNNNN.sst      SSTable
  kDescriptorFile,  // MANIFEST-NNNNNN log of VersionEdits
  kCurrentFile,     // CURRENT         names the live MANIFEST
  kLockFile,        // LOCK
  kTempFile,        // NNNNNN.dbtmp    staging file for CURRENT
};

std::string TableFileName(std::string_view dbname, uint64_t number);
std::string LogFileName(std::string_view dbname, uint64_t number);
std::string DescriptorFileName(std::string_view dbname, uint64_t number);
std::string TempFileName(std::string_view dbname, uint64_t number);
std::string CurrentFileName(std::string_view dbname);
std::string LockFileName(std::string_view dbname);

// Parses a file name (not a path). Returns false for names Talus did not
// create.
bool ParseFileName(std::string_view name, uint64_t* number, FileType* type);

// Atomically points CURRENT at MANIFEST-<manifest_number>. It writes the
// contents to a temp file, fsyncs it, renames it over CURRENT, then fsyncs
// the directory. A crash leaves either the old or the new CURRENT, never a
// partial one.
Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number);

}  // namespace talus
