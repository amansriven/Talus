#pragma once

// Minimal POSIX file I/O. Every file descriptor is owned by a FileDescriptor.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Talus/status.h"

namespace talus {

// Owns a POSIX file descriptor and closes it on destruction.
class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor();

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.Release()) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  // Closes the descriptor, reporting any error from close(2).
  Status Close(const std::string& name);

 private:
  int fd_ = -1;
};

// An append-only file with a fixed-size write buffer that is allocated once,
// so steady-state appends do not allocate.
class WritableFile {
  struct PrivateTag {};

 public:
  // Creates `path`, truncating any existing file.
  static Status Create(const std::string& path, std::unique_ptr<WritableFile>* result);

  // Use Create(). (Public only so std::make_unique can call it.)
  WritableFile(PrivateTag, FileDescriptor fd, std::string path);

  // Flushes buffered data (errors ignored; call Close() to observe them).
  ~WritableFile();

  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  Status Append(std::string_view data);
  // Hands buffered data to the OS. Survives a process crash, but not a
  // power failure or kernel crash.
  Status Flush();
  // Flush(), then make the data durable: fdatasync on Linux,
  // fcntl(F_FULLFSYNC) on macOS (plain fsync there does not flush the drive's
  // write cache).
  Status Sync();
  Status Close();

  // Total bytes appended.
  uint64_t size() const { return size_; }

 private:
  static constexpr size_t kBufferSize = 64 * 1024;

  Status WriteRaw(const char* data, size_t n);

  FileDescriptor fd_;
  const std::string path_;
  std::unique_ptr<char[]> buffer_;
  size_t buffered_ = 0;
  uint64_t size_ = 0;
};

// A buffered sequential reader.
class SequentialFile {
  struct PrivateTag {};

 public:
  static Status Open(const std::string& path, std::unique_ptr<SequentialFile>* result);

  // Use Open(). (Public only so std::make_unique can call it.)
  SequentialFile(PrivateTag, FileDescriptor fd, std::string path, uint64_t size);

  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  // Reads up to `n` bytes into `dst`. *bytes_read is less than `n` only at end
  // of file.
  Status Read(size_t n, char* dst, size_t* bytes_read);

  // The file's size when it was opened, and the current read offset.
  uint64_t size() const { return size_; }
  uint64_t position() const { return position_; }

 private:
  static constexpr size_t kBufferSize = 64 * 1024;

  Status ReadRaw(char* dst, size_t n, size_t* bytes_read);

  FileDescriptor fd_;
  const std::string path_;
  const uint64_t size_;
  uint64_t position_ = 0;
  std::unique_ptr<char[]> buffer_;
  size_t buffer_pos_ = 0;
  size_t buffer_len_ = 0;
};

// fsyncs a directory, making the creation, deletion or renaming of entries
// in it durable.
Status SyncDirectory(const std::string& dir);

}  // namespace talus
