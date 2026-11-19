#pragma once

// The engine behind the load tester: many connections, each keeping a fixed
// number of requests in flight, measuring the latency of every reply.
//
// Concurrency vs. volume (the spec asks to keep them apart):
//   concurrency = connections x depth   requests in flight at any moment
//   volume      = whatever completes in `duration` (reported as ops/sec)
//
// Closed loop: a connection sends its next request only when a reply comes
// back. Simple and stable, but it hides some queueing delay when the server
// stalls ("coordinated omission"): see docs and benchmarks/results.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cachegrid/LatencyHistogram.h"
#include "cachegrid/LRUCache.h"  // Millis
#include "cachegrid/Workload.h"

namespace cachegrid {

class Config;

struct LoadOptions {
  std::string host;
  std::uint16_t port;
  std::size_t threads;            // client event-loop threads
  std::size_t connections;        // total connections (spread over threads)
  std::size_t depth;              // requests in flight per connection
  Millis warmup;                  // run, but don't record, for this long first
  Millis duration;                // then record for this long
  Millis connect_timeout;
  Millis poll_tick;               // event-loop wake-up interval (checks for stop)
  Millis timeline_interval;       // 0 = no per-interval timeline
  std::size_t read_chunk_bytes;
  std::size_t events_per_poll;
  std::size_t max_reply_bytes;
  unsigned histogram_bits;
  std::uint64_t seed;
  // Cache-aside: after a GET miss, SET the key (as an application would after
  // reading it from its database). Without this, an evicted key stays
  // missing until the workload happens to write it again.
  bool refill_on_miss;
};

struct IntervalResult {
  std::uint64_t ops = 0;
  std::uint64_t errors = 0;
  LatencyHistogram latency;
  explicit IntervalResult(unsigned bits) : latency(bits) {}
};

struct LoadResult {
  explicit LoadResult(unsigned bits) : all(bits), get(bits), set(bits) {}

  LatencyHistogram all, get, set;  // nanoseconds
  std::uint64_t ops = 0, gets = 0, sets = 0, dels = 0;
  std::uint64_t hits = 0, misses = 0, errors = 0;
  std::uint64_t refills = 0;  // SETs issued by refill_on_miss (included in sets)
  std::uint64_t connections_failed = 0;  // couldn't connect, or dropped mid-run
  double seconds = 0;                    // measured window
  double client_cpu_seconds = 0;         // this process's CPU time in the window
  std::vector<IntervalResult> timeline;

  double ops_per_sec() const { return seconds > 0 ? static_cast<double>(ops) / seconds : 0; }
};

// Timed run: warmup, then measure for `duration`.
LoadResult run_load(const LoadOptions& opts, const Workload& workload);

// Sends each op in `ops` once (e.g. preloading every key) as fast as the
// connections allow. Returns the number of error replies.
std::uint64_t run_ops_once(const LoadOptions& opts, const Workload& workload, const std::vector<Op>& ops);

// Fills everything except connections/depth from `bench.load.*` keys.
LoadOptions load_options_from_config(const Config& cfg);

}  // namespace cachegrid
