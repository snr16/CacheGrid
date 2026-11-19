#include "cachegrid/ProcessStats.h"

#include <sys/resource.h>

namespace cachegrid {

double process_cpu_seconds() {
  rusage u{};
  ::getrusage(RUSAGE_SELF, &u);
  auto sec = [](const timeval& t) { return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6; };
  return sec(u.ru_utime) + sec(u.ru_stime);
}

std::uint64_t process_peak_rss_bytes() {
  rusage u{};
  ::getrusage(RUSAGE_SELF, &u);
  // ru_maxrss is reported in bytes on macOS but in kilobytes on Linux.
#ifdef __APPLE__
  return static_cast<std::uint64_t>(u.ru_maxrss);
#else
  return static_cast<std::uint64_t>(u.ru_maxrss) * 1024;
#endif
}

}  // namespace cachegrid
