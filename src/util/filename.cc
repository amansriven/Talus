#include "util/filename.h"

#include <cinttypes>
#include <cstdio>
#include <limits>
#include <memory>

#include "util/file.h"

namespace talus {
namespace {

std::string NumberedFileName(std::string_view dbname, uint64_t number, const char* suffix) {
  char name[48];
  std::snprintf(name, sizeof(name), "/%06" PRIu64 ".%s", number, suffix);
  std::string result(dbname);
  result += name;
  return result;
}

// Parses a non-empty run of decimal digits at the front of *s. Rejects
// numbers that overflow uint64_t.
bool ConsumeNumber(std::string_view* s, uint64_t* number) {
  uint64_t v = 0;
  size_t i = 0;
  for (; i < s->size() && (*s)[i] >= '0' && (*s)[i] <= '9'; ++i) {
    const auto digit = static_cast<uint64_t>((*s)[i] - '0');
    if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    v = v * 10 + digit;
  }
  if (i == 0) {
    return false;
  }
  *number = v;
  s->remove_prefix(i);
  return true;
}

}  // namespace

std::string TableFileName(std::string_view dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "sst");
}

std::string LogFileName(std::string_view dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "log");
}

std::string TempFileName(std::string_view dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "dbtmp");
}

std::string DescriptorFileName(std::string_view dbname, uint64_t number) {
  char name[48];
  std::snprintf(name, sizeof(name), "/MANIFEST-%06" PRIu64, number);
  return std::string(dbname) + name;
}

std::string CurrentFileName(std::string_view dbname) { return std::string(dbname) + "/CURRENT"; }

std::string LockFileName(std::string_view dbname) { return std::string(dbname) + "/LOCK"; }

bool ParseFileName(std::string_view name, uint64_t* number, FileType* type) {
  if (name == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrentFile;
    return true;
  }
  if (name == "LOCK") {
    *number = 0;
    *type = FileType::kLockFile;
    return true;
  }
  if (name.starts_with("MANIFEST-")) {
    name.remove_prefix(9);
    *type = FileType::kDescriptorFile;
    return ConsumeNumber(&name, number) && name.empty();
  }
  if (!ConsumeNumber(&name, number)) {
    return false;
  }
  if (name == ".log") {
    *type = FileType::kLogFile;
  } else if (name == ".sst") {
    *type = FileType::kTableFile;
  } else if (name == ".dbtmp") {
    *type = FileType::kTempFile;
  } else {
    return false;
  }
  return true;
}

Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number) {
  // CURRENT holds "MANIFEST-NNNNNN\n".
  std::string contents = DescriptorFileName(dbname, manifest_number);
  contents = contents.substr(dbname.size() + 1) + "\n";
  const std::string tmp = TempFileName(dbname, manifest_number);

  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Create(tmp, &file);
  if (s.ok()) {
    s = file->Append(contents);
  }
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (s.ok()) {
    s = RenameFile(tmp, CurrentFileName(dbname));
  }
  if (s.ok()) {
    s = SyncDirectory(dbname);
  } else {
    (void)RemoveFile(tmp);  // Best effort; recovery removes stray temp files anyway.
  }
  return s;
}

}  // namespace talus
