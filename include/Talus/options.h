#pragma once

namespace talus {

struct WriteOptions {
  // If true, the write is durable (its WAL record fsynced) before Write()
  // returns. If false, a process crash cannot lose it, but a machine crash
  // can lose the most recent writes.
  bool sync = false;
};

}  // namespace talus
