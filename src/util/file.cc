#include "util/file.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

namespace talus {
namespace {

Status PosixError(const std::string& context, int err) {
  return Status::IOError(context, std::generic_category().message(err));
}

// Makes everything written to `fd` durable.
Status SyncFd(int fd, const std::string& name) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return Status::OK();
  }
  // Some file systems do not support F_FULLFSYNC; fall back to fsync.
  if (::fsync(fd) == 0) {
    return Status::OK();
  }
#elif defined(__linux__)
  if (::fdatasync(fd) == 0) {
    return Status::OK();
  }
#else
  if (::fsync(fd) == 0) {
    return Status::OK();
  }
#endif
  return PosixError(name, errno);
}

}  // namespace

FileDescriptor::~FileDescriptor() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.Release();
  }
  return *this;
}

Status FileDescriptor::Close(const std::string& name) {
  if (fd_ < 0) {
    return Status::OK();
  }
  const int fd = Release();
  // Do not retry on EINTR: on Linux the descriptor is released regardless.
  if (::close(fd) != 0) {
    return PosixError(name, errno);
  }
  return Status::OK();
}

Status WritableFile::Create(const std::string& path, std::unique_ptr<WritableFile>* result) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return PosixError(path, errno);
  }
  *result = std::make_unique<WritableFile>(PrivateTag{}, FileDescriptor(fd), path);
  return Status::OK();
}

WritableFile::WritableFile(PrivateTag /*tag*/, FileDescriptor fd, std::string path)
    : fd_(std::move(fd)),
      path_(std::move(path)),
      buffer_(std::make_unique_for_overwrite<char[]>(kBufferSize)) {}

WritableFile::~WritableFile() {
  if (fd_.valid()) {
    (void)Flush();  // Best effort; Close() reports errors.
  }
}

Status WritableFile::Append(std::string_view data) {
  const char* p = data.data();
  size_t n = data.size();
  size_ += n;

  const size_t copy = std::min(n, kBufferSize - buffered_);
  if (copy > 0) {
    std::memcpy(buffer_.get() + buffered_, p, copy);
    buffered_ += copy;
    p += copy;
    n -= copy;
  }
  if (n == 0) {
    return Status::OK();
  }
  // The buffer is full and data remains.
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  if (n < kBufferSize) {
    std::memcpy(buffer_.get(), p, n);
    buffered_ = n;
    return Status::OK();
  }
  return WriteRaw(p, n);  // Large writes bypass the buffer.
}

Status WritableFile::Flush() {
  Status s = WriteRaw(buffer_.get(), buffered_);
  buffered_ = 0;
  return s;
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  return SyncFd(fd_.get(), path_);
}

Status WritableFile::Close() {
  Status s = Flush();
  Status close_status = fd_.Close(path_);
  return s.ok() ? close_status : s;
}

Status WritableFile::WriteRaw(const char* data, size_t n) {
  while (n > 0) {
    const ssize_t written = ::write(fd_.get(), data, n);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PosixError(path_, errno);
    }
    data += written;
    n -= static_cast<size_t>(written);
  }
  return Status::OK();
}

Status SequentialFile::Open(const std::string& path, std::unique_ptr<SequentialFile>* result) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(path, errno);
  }
  FileDescriptor owned(fd);
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return PosixError(path, errno);
  }
  *result = std::make_unique<SequentialFile>(PrivateTag{}, std::move(owned), path,
                                             static_cast<uint64_t>(st.st_size));
  return Status::OK();
}

SequentialFile::SequentialFile(PrivateTag /*tag*/, FileDescriptor fd, std::string path,
                               uint64_t size)
    : fd_(std::move(fd)),
      path_(std::move(path)),
      size_(size),
      buffer_(std::make_unique_for_overwrite<char[]>(kBufferSize)) {}

Status SequentialFile::Read(size_t n, char* dst, size_t* bytes_read) {
  size_t total = 0;
  while (n > 0) {
    if (buffer_pos_ == buffer_len_) {
      size_t got = 0;
      Status s =
          n >= kBufferSize ? ReadRaw(dst, n, &got) : ReadRaw(buffer_.get(), kBufferSize, &got);
      if (!s.ok()) {
        *bytes_read = total;
        return s;
      }
      if (got == 0) {
        break;  // End of file.
      }
      if (n >= kBufferSize) {  // Large reads bypass the buffer.
        dst += got;
        n -= got;
        total += got;
        position_ += got;
        continue;
      }
      buffer_pos_ = 0;
      buffer_len_ = got;
    }
    const size_t copy = std::min(n, buffer_len_ - buffer_pos_);
    std::memcpy(dst, buffer_.get() + buffer_pos_, copy);
    buffer_pos_ += copy;
    dst += copy;
    n -= copy;
    total += copy;
    position_ += copy;
  }
  *bytes_read = total;
  return Status::OK();
}

Status SequentialFile::ReadRaw(char* dst, size_t n, size_t* bytes_read) {
  while (true) {
    const ssize_t r = ::read(fd_.get(), dst, n);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      *bytes_read = 0;
      return PosixError(path_, errno);
    }
    *bytes_read = static_cast<size_t>(r);
    return Status::OK();
  }
}

Status RandomAccessFile::Open(const std::string& path, Mode mode,
                              std::unique_ptr<RandomAccessFile>* result) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(path, errno);
  }
  FileDescriptor owned(fd);
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return PosixError(path, errno);
  }
  const auto size = static_cast<uint64_t>(st.st_size);
  const char* mapping = nullptr;
  if (mode == Mode::kMmap && size > 0) {
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
      return PosixError(path, errno);
    }
    mapping = static_cast<const char*>(p);
    // The mapping stays valid after close, and reads never use the fd, so a
    // mapped table holds no file descriptor: thousands of open tables do not
    // run into RLIMIT_NOFILE.
    owned = FileDescriptor();
  }
  *result = std::make_unique<RandomAccessFile>(PrivateTag{}, std::move(owned), path, size, mapping);
  return Status::OK();
}

RandomAccessFile::RandomAccessFile(PrivateTag /*tag*/, FileDescriptor fd, std::string path,
                                   uint64_t size, const char* mapping)
    : fd_(std::move(fd)), path_(std::move(path)), size_(size), mapping_(mapping) {}

RandomAccessFile::~RandomAccessFile() {
  if (mapping_ != nullptr) {
    ::munmap(const_cast<char*>(mapping_), size_);
  }
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string_view* result,
                              char* scratch) const {
  if (offset >= size_) {
    *result = {};
    return Status::OK();
  }
  n = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
  if (mapping_ != nullptr) {
    *result = std::string_view(mapping_ + offset, n);
    return Status::OK();
  }
  size_t done = 0;
  while (done < n) {
    const ssize_t r =
        ::pread(fd_.get(), scratch + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PosixError(path_, errno);
    }
    if (r == 0) {
      break;  // The file shrank since it was opened.
    }
    done += static_cast<size_t>(r);
  }
  *result = std::string_view(scratch, done);
  return Status::OK();
}

Status SyncDirectory(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(dir, errno);
  }
  FileDescriptor owned(fd);
  Status s = SyncFd(fd, dir);
  Status close_status = owned.Close(dir);
  return s.ok() ? close_status : s;
}

}  // namespace talus

namespace talus {

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return PosixError(dir, errno);
  }
  return Status::OK();
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status GetChildren(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return PosixError(dir, errno);
  }
  while (const struct dirent* entry = ::readdir(d)) {
    const std::string_view name = entry->d_name;
    if (name != "." && name != "..") {
      names->emplace_back(name);
    }
  }
  ::closedir(d);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) {
    return PosixError(path, errno);
  }
  return Status::OK();
}

Status RenameFile(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return PosixError(from, errno);
  }
  return Status::OK();
}

Status FileLock::Acquire(const std::string& path, std::unique_ptr<FileLock>* lock) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return PosixError(path, errno);
  }
  FileDescriptor owned(fd);
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      return Status::Busy(path, "database is locked by another instance");
    }
    return PosixError(path, errno);
  }
  *lock = std::make_unique<FileLock>(PrivateTag{}, std::move(owned));
  return Status::OK();
}

}  // namespace talus
