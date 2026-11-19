#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cachegrid {

// Records latencies (any unit; the load tester uses nanoseconds) in a
// fixed-size table so millions of samples cost almost no memory, and answers
// percentile queries (p50, p95, p99...) with bounded relative error.
//
// How it works ("log-linear" buckets, the idea behind HdrHistogram):
//   - Values below 2^precision_bits get one bucket each (exact).
//   - Above that, each power-of-two range [2^k, 2^(k+1)) is split into
//     2^(precision_bits-1) equal sub-buckets.
// So a bucket is never wider than ~1 / 2^(precision_bits-1) of the values it
// holds: with 7 bits, every reported value is within ~1.6% of the true one.
// Recording is O(1): a couple of bit operations and an increment.
//
// Not thread-safe: each thread records into its own histogram, and they are
// merged at the end.
class LatencyHistogram {
 public:
  // precision_bits: 1..20. More bits = finer buckets, more memory.
  explicit LatencyHistogram(unsigned precision_bits);

  void record(std::uint64_t value);
  void merge(const LatencyHistogram& other);  // same precision required
  void reset();

  std::uint64_t count() const { return count_; }
  std::uint64_t min() const { return count_ ? min_ : 0; }
  std::uint64_t max() const { return max_; }
  double mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

  // Smallest bucket upper bound such that at least `p` percent of samples
  // are <= it (p in 0..100). Conservative: never under-reports.
  std::uint64_t percentile(double p) const;

 private:
  std::size_t index_of(std::uint64_t v) const;
  std::uint64_t highest_value_in(std::size_t index) const;

  unsigned bits_;
  std::uint64_t sub_count_;  // 2^bits
  std::uint64_t half_;       // 2^(bits-1)
  std::vector<std::uint64_t> buckets_;
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace cachegrid
