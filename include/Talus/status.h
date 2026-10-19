#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace talus {

// The result of a fallible engine operation.
//
// An OK status (and any status created without a message) holds no heap
// memory, so returning one on a hot path never allocates.
class [[nodiscard]] Status {
 public:
  enum class Code : unsigned char {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4,
    kBusy = 5,
  };

  Status() noexcept = default;
  ~Status() = default;

  Status(const Status& other);
  Status& operator=(const Status& other);
  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) noexcept = default;

  static Status OK() noexcept { return {}; }
  static Status NotFound(std::string_view msg = {}, std::string_view msg2 = {}) {
    return {Code::kNotFound, msg, msg2};
  }
  static Status Corruption(std::string_view msg = {}, std::string_view msg2 = {}) {
    return {Code::kCorruption, msg, msg2};
  }
  static Status IOError(std::string_view msg = {}, std::string_view msg2 = {}) {
    return {Code::kIOError, msg, msg2};
  }
  static Status InvalidArgument(std::string_view msg = {}, std::string_view msg2 = {}) {
    return {Code::kInvalidArgument, msg, msg2};
  }
  static Status Busy(std::string_view msg = {}, std::string_view msg2 = {}) {
    return {Code::kBusy, msg, msg2};
  }

  bool ok() const noexcept { return code_ == Code::kOk; }
  bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
  bool IsIOError() const noexcept { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const noexcept { return code_ == Code::kInvalidArgument; }
  bool IsBusy() const noexcept { return code_ == Code::kBusy; }

  Code code() const noexcept { return code_; }

  // The detail message, or an empty view if there is none.
  std::string_view message() const noexcept {
    return msg_ ? std::string_view(*msg_) : std::string_view();
  }

  // A human-readable form such as "Corruption: bad block checksum".
  std::string ToString() const;

 private:
  Status(Code code, std::string_view msg, std::string_view msg2);

  Code code_ = Code::kOk;
  std::unique_ptr<std::string> msg_;  // Null when there is no message.
};

}  // namespace talus
