#include "cachegrid/LatencyHistogram.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cachegrid {

namespace {
constexpr unsigned kMaxPrecisionBits = 20;  // keeps the table under ~1M buckets
constexpr unsigned kValueBits = 64;         // width of a recorded value

unsigned highest_bit(std::uint64_t v) {
  unsigned b = 0;
  while (v >>= 1) ++b;
  return b;
}
}  // namespace

LatencyHistogram::LatencyHistogram(unsigned precision_bits)
    : bits_(precision_bits), sub_count_(std::uint64_t{1} << precision_bits), half_(sub_count_ / 2) {
  if (precision_bits < 1 || precision_bits > kMaxPrecisionBits) {
    throw std::invalid_argument("precision_bits must be 1..20");
  }
  // One "exact" range of sub_count_ buckets, then half_ buckets for each
  // remaining power of two up to 2^64.
  buckets_.assign(static_cast<std::size_t>(sub_count_ + (kValueBits - bits_ + 1) * half_), 0);
}

std::size_t LatencyHistogram::index_of(std::uint64_t v) const {
  if (v < sub_count_) return static_cast<std::size_t>(v);
  // shift = how many low bits are dropped for this power-of-two range.
  const unsigned shift = highest_bit(v) - (bits_ - 1);
  const std::uint64_t sub = v >> shift;  // in [half_, sub_count_)
  return static_cast<std::size_t>(shift * half_ + sub);
}

std::uint64_t LatencyHistogram::highest_value_in(std::size_t index) const {
  const std::uint64_t i = index;
  if (i < sub_count_) return i;
  const std::uint64_t shift = i / half_ - 1;
  const std::uint64_t sub = i - shift * half_;
  return ((sub + 1) << shift) - 1;
}

void LatencyHistogram::record(std::uint64_t value) {
  ++buckets_[index_of(value)];
  min_ = count_ == 0 ? value : std::min(min_, value);
  max_ = std::max(max_, value);
  ++count_;
  sum_ += value;
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
  if (other.bits_ != bits_) throw std::invalid_argument("histogram precision mismatch");
  for (std::size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += other.buckets_[i];
  if (other.count_) {
    min_ = count_ == 0 ? other.min_ : std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
  count_ += other.count_;
  sum_ += other.sum_;
}

void LatencyHistogram::reset() {
  std::fill(buckets_.begin(), buckets_.end(), 0);
  count_ = sum_ = min_ = max_ = 0;
}

std::uint64_t LatencyHistogram::percentile(double p) const {
  if (count_ == 0) return 0;
  const double clamped = std::min(100.0, std::max(0.0, p));
  // Number of samples that must be at or below the answer (at least 1).
  const auto target = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(count_))));
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    seen += buckets_[i];
    if (seen >= target) return std::min(highest_value_in(i), max_);
  }
  return max_;
}

}  // namespace cachegrid
