#include "util/port.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace talus {

void PinCurrentThreadToCpus(const std::vector<int>& cpus) {
#if defined(__linux__)
  if (cpus.empty()) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu : cpus) {
    if (cpu >= 0 && cpu < CPU_SETSIZE) {
      CPU_SET(cpu, &set);
    }
  }
  // Failure (e.g. a CPU outside the cgroup's set) leaves the thread unpinned,
  // which is only slower.
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpus;
#endif
}

}  // namespace talus
