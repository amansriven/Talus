#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace talus {

// "<dbname>/NNNNNN.sst" (at least six digits, zero-padded).
std::string TableFileName(std::string_view dbname, uint64_t number);

// "<dbname>/NNNNNN.log"
std::string LogFileName(std::string_view dbname, uint64_t number);

}  // namespace talus
