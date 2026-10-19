#include "Talus/status.h"

namespace talus {

Status::Status(Code code, std::string_view msg, std::string_view msg2) : code_(code) {
  if (msg.empty() && msg2.empty()) {
    return;
  }
  msg_ = std::make_unique<std::string>(msg);
  if (!msg2.empty()) {
    msg_->append(": ");
    msg_->append(msg2);
  }
}

Status::Status(const Status& other)
    : code_(other.code_), msg_(other.msg_ ? std::make_unique<std::string>(*other.msg_) : nullptr) {}

Status& Status::operator=(const Status& other) {
  if (this != &other) {
    code_ = other.code_;
    msg_ = other.msg_ ? std::make_unique<std::string>(*other.msg_) : nullptr;
  }
  return *this;
}

std::string Status::ToString() const {
  std::string_view name;
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      name = "NotFound";
      break;
    case Code::kCorruption:
      name = "Corruption";
      break;
    case Code::kIOError:
      name = "IOError";
      break;
    case Code::kInvalidArgument:
      name = "InvalidArgument";
      break;
    case Code::kBusy:
      name = "Busy";
      break;
  }
  std::string result(name);
  if (msg_) {
    result.append(": ");
    result.append(*msg_);
  }
  return result;
}

}  // namespace talus
