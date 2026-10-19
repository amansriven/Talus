#pragma once

#include <cassert>

namespace talus {

// Keeps resources alive (a pinned block-cache entry, an open table) and
// releases them when destroyed or Reset(). Move-only, and holds at most
// kMaxCleanups cleanups inline, so pinning never allocates.
class Pin {
 public:
  using CleanupFn = void (*)(void* arg1, void* arg2);

  Pin() = default;
  ~Pin() { Reset(); }

  Pin(Pin&& other) noexcept { TakeFrom(other); }
  Pin& operator=(Pin&& other) noexcept {
    if (this != &other) {
      Reset();
      TakeFrom(other);
    }
    return *this;
  }
  Pin(const Pin&) = delete;
  Pin& operator=(const Pin&) = delete;

  // Registers fn(arg1, arg2) to run on release. Cleanups run in reverse
  // order of registration.
  void Add(CleanupFn fn, void* arg1, void* arg2) {
    assert(count_ < kMaxCleanups);
    cleanups_[count_++] = {fn, arg1, arg2};
  }

  void Reset() {
    while (count_ > 0) {
      --count_;
      cleanups_[count_].fn(cleanups_[count_].arg1, cleanups_[count_].arg2);
    }
  }

  bool empty() const { return count_ == 0; }

 private:
  static constexpr int kMaxCleanups = 2;  // A data block and its table.

  struct Cleanup {
    CleanupFn fn;
    void* arg1;
    void* arg2;
  };

  void TakeFrom(Pin& other) {
    count_ = other.count_;
    for (int i = 0; i < count_; ++i) {
      cleanups_[i] = other.cleanups_[i];
    }
    other.count_ = 0;
  }

  Cleanup cleanups_[kMaxCleanups] = {};
  int count_ = 0;
};

}  // namespace talus
