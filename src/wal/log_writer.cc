#include "wal/log_writer.h"

#include <limits>
#include <utility>

#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/log_format.h"

namespace talus::log {
namespace {

uint32_t TypeCrc(RecordType type) {
  const auto byte = static_cast<char>(type);
  return crc32c::Value(&byte, 1);
}

}  // namespace

Writer::Writer(std::unique_ptr<WritableFile> dest)
    : dest_(std::move(dest)), type_crc_(TypeCrc(kFullType)) {}

Status Writer::AddRecord(std::string_view payload) {
  if (payload.size() > std::numeric_limits<uint32_t>::max()) {
    return Status::InvalidArgument("log record larger than 4 GiB");
  }
  // The header is encoded on the stack; the file's write buffer is allocated
  // once, so steady-state appends do not allocate.
  char header[kHeaderSize];
  const uint32_t crc = crc32c::Extend(type_crc_, payload.data(), payload.size());
  EncodeFixed32(header, crc32c::Mask(crc));
  EncodeFixed32(header + 4, static_cast<uint32_t>(payload.size()));
  header[8] = static_cast<char>(kFullType);

  Status s = dest_->Append(std::string_view(header, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(payload);
  }
  if (s.ok()) {
    s = dest_->Flush();
  }
  return s;
}

}  // namespace talus::log
