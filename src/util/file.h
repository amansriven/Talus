#pragma once

// Minimal POSIX file I/O. Every file descriptor is owned by a FileDescriptor.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// A read-only file for positional reads (SSTables), using either pread(2)
// or a read-only mmap of the whole file. Thread-safe.
class RandomAccessFile {
  struct PrivateTag {};

 public:
  enum class Mode { kPread, kMmap };

  static Status Open(const std::string& path, Mode mode, std::unique_ptr<RandomAccessFile>* result);

  // Use Open(). (Public only so std::make_unique can call it.)
  RandomAccessFile(PrivateTag, FileDescriptor fd, std::string path, uint64_t size,
                   const char* mapping);
  ~RandomAccessFile();

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Reads up to `n` bytes at `offset`; *result is shorter only at end of
  // file. With pread the bytes are copied into `scratch` (room for `n`) and
  // *result points there. With mmap *result points into the mapping
  // (zero-copy) and `scratch` is unused.
  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const;

  uint64_t size() const { return size_; }
  // True if Read() returns pointers into a mapping that lives as long as
  // this object, so callers need not copy.
  bool is_mmap() const { return mapping_ != nullptr; }

 private:
  FileDescriptor fd_;
  const std::string path_;
  const uint64_t size_;
  const char* const mapping_;  // Null in pread mode (and for empty files).
};

// fsyncs a directory, making the creation, deletion or renaming of entries
// in it durable.
Status SyncDirectory(const std::string& dir);

Status CreateDirIfMissing(const std::string& dir);
bool FileExists(const std::string& path);
// The names (not paths) of the entries in `dir`, excluding "." and "..".
Status GetChildren(const std::string& dir, std::vector<std::string>* names);
Status RemoveFile(const std::string& path);
// Atomically replaces `to` with `from` (rename(2)).
Status RenameFile(const std::string& from, const std::string& to);

// An exclusive advisory lock on a file (flock), held until destruction.
// Prevents two DB instances, in this or another process, from opening the
// same database.
class FileLock {
  struct PrivateTag {};

 public:
  // Returns Busy if another holder has the lock.
  static Status Acquire(const std::string& path, std::unique_ptr<FileLock>* lock);
  FileLock(PrivateTag, FileDescriptor fd) : fd_(std::move(fd)) {}

 private:
  FileDescriptor fd_;  // Closing it releases the lock.
};

}  // namespace talus
