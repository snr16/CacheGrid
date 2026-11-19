#pragma once

// Workloads for the load tester: where the requests come from.
//
//   uniform  synthetic: every key equally likely
//   zipf     synthetic: a few "hot" keys get most requests (like real traffic)
//   trace    replay of a real production cache trace (Twitter, CSV format)
//
// A workload hands out operations by index. Synthetic workloads compute them
// on the fly; a trace is loaded into memory once and replayed in order
// (wrapping around at the end).

#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace cachegrid {

class Config;

struct Op {
  enum class Type : std::uint8_t { Get, Set, Del };
  Type type;
  std::uint32_t key;         // index into the workload's key space
  std::uint32_t value_size;  // bytes (SET only)
  std::uint32_t ttl_s;       // seconds, 0 = no expiry (SET only)
};

// YCSB-style Zipfian generator (Gray et al., "Quickly generating
// billion-record synthetic databases"). Returns ranks 0..n-1; rank 0 is the
// most popular. theta near 1 = very skewed, near 0 = almost uniform.
class ZipfianGenerator {
 public:
  ZipfianGenerator(std::uint64_t n, double theta);
  std::uint64_t next(std::mt19937_64& rng) const;

 private:
  std::uint64_t n_;
  double alpha_, zetan_, eta_, half_pow_theta_;
};

class Workload {
 public:
  virtual ~Workload() = default;

  // Number of distinct keys and the name of key i (what goes on the wire).
  virtual std::size_t key_count() const = 0;
  virtual const std::string& key_name(std::uint32_t key) const = 0;

  // The op to issue. `seq` is a global request counter; `rng` is the calling
  // thread's own random generator (synthetic workloads use it, traces don't).
  virtual Op next(std::uint64_t seq, std::mt19937_64& rng) const = 0;

  // SETs that load every key before measuring (so GETs can hit).
  virtual std::vector<Op> preload_ops() const = 0;

  // Largest value any op asks for (sizes the shared filler buffer).
  virtual std::uint32_t max_value_size() const = 0;

  // Value size to write back after a GET miss on `key` (cache-aside refill).
  virtual std::uint32_t refill_size(std::uint32_t key) const = 0;

  virtual std::string describe() const = 0;
};

struct SyntheticOptions {
  enum class Distribution { Uniform, Zipf };
  Distribution distribution;
  std::size_t keys;
  std::uint32_t value_size;
  unsigned get_percent;  // rest are SETs
  double zipf_theta;
};

std::unique_ptr<Workload> make_synthetic_workload(const SyntheticOptions& opts);

// Trace replay. Each line: timestamp,key,key_size,value_size,client,op,ttl
// (Twitter cache-trace format). Mapping:
//   get, gets                                   -> GET
//   set, add, replace, cas, append, prepend,
//   incr, decr                                  -> SET (value of the recorded size)
//   delete                                      -> DEL
// Values larger than max_value_bytes are clamped to it (counted in stats).
struct TraceStats {
  std::size_t lines = 0;
  std::size_t gets = 0, sets = 0, dels = 0, skipped = 0, clamped = 0;
};
std::unique_ptr<Workload> load_trace_workload(std::istream& in, std::size_t max_lines,
                                              std::uint32_t max_value_bytes, const std::string& name,
                                              TraceStats& stats);

// Builds the workload described by `bench.workload.*` keys.
std::unique_ptr<Workload> make_workload_from_config(const Config& cfg);

}  // namespace cachegrid
