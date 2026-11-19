#pragma once

#include <cstdint>

namespace cachegrid {

// CPU time (user + system) this process has used so far, in seconds.
double process_cpu_seconds();

// Largest resident memory (RAM actually in use) this process has reached, in bytes.
std::uint64_t process_peak_rss_bytes();

}  // namespace cachegrid
