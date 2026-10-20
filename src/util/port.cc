#include "util/port.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
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

void SetCurrentThreadNice(int nice) {
#if defined(__linux__)
  if (nice != 0) {
    // On Linux, PRIO_PROCESS with a thread ID sets that thread's nice value.
    // Failure (e.g. lowering below 0 without privilege) only costs latency.
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
  }
#else
  (void)nice;
#endif
}

}  // namespace talus
