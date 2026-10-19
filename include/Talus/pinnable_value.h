#pragma once

#include <cassert>
#include <string_view>
#include <utility>

namespace talus {

// Keeps resources alive (a pinned block-cache entry, an open table, a
// memtable) and releases them when destroyed or Reset(). Move-only, and
// holds at most two cleanups inline, so pinning never allocates. Used by
// PinnableValue; also used internally by the engine.
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

// The result of DB::Get(..., PinnableValue*): a view straight into the
// engine's memory (a cached SSTable block, a memory-mapped file, or a
// memtable), kept valid by a pin rather than copied. Destroying the
// PinnableValue, calling Reset(), or reusing it for another Get releases the
// pin. On a block-cache or memtable hit, Get into a PinnableValue performs
// no heap allocation.
//
// It must not outlive the DB.
class PinnableValue {
 public:
  PinnableValue() = default;
  PinnableValue(PinnableValue&&) noexcept = default;
  PinnableValue& operator=(PinnableValue&&) noexcept = default;

  std::string_view value() const { return value_; }

  void Reset() {
    value_ = {};
    pin_.Reset();
  }

  // Engine use: sets the value and takes ownership of what keeps it alive.
  void PinTo(std::string_view value, Pin&& pin) {
    pin_ = std::move(pin);
    value_ = value;
  }

 private:
  std::string_view value_;
  Pin pin_;
};

}  // namespace talus
