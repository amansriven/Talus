#include "util/filename.h"

#include <cinttypes>
#include <cstdio>

namespace talus {
namespace {

std::string NumberedFileName(std::string_view dbname, uint64_t number, const char* suffix) {
  char name[32];
  std::snprintf(name, sizeof(name), "/%06" PRIu64 ".%s", number, suffix);
  std::string result(dbname);
  result += name;
  return result;
}

}  // namespace

std::string TableFileName(std::string_view dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "sst");
}

std::string LogFileName(std::string_view dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "log");
}

}  // namespace talus
