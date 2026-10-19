#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

namespace talus::test {

// A fresh directory under the system temp dir, removed on destruction.
class ScopedTempDir {
 public:
  ScopedTempDir() {
    static std::atomic<int> counter{0};
    // Relaxed: only uniqueness matters. The pid separates test processes.
    path_ = std::filesystem::temp_directory_path() /
            ("talus_test_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  std::string path() const { return path_.string(); }
  std::string File(std::string_view name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace talus::test
