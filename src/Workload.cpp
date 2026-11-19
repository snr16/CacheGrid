#include "cachegrid/Workload.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "cachegrid/Config.h"

namespace cachegrid {

// ---- Zipfian ----------------------------------------------------------------------

ZipfianGenerator::ZipfianGenerator(std::uint64_t n, double theta) : n_(n) {
  if (n == 0) throw std::invalid_argument("zipf needs at least one item");
  if (theta <= 0.0 || theta >= 1.0) throw std::invalid_argument("zipf theta must be in (0, 1)");
  // zeta(n) = sum of 1/i^theta: the normalising constant. O(n), computed once.
  zetan_ = 0.0;
  for (std::uint64_t i = 1; i <= n; ++i) zetan_ += 1.0 / std::pow(static_cast<double>(i), theta);
  const double zeta2 = 1.0 + std::pow(0.5, theta);
  alpha_ = 1.0 / (1.0 - theta);
  eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) / (1.0 - zeta2 / zetan_);
  half_pow_theta_ = std::pow(0.5, theta);
}

std::uint64_t ZipfianGenerator::next(std::mt19937_64& rng) const {
  const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
  const double uz = u * zetan_;
  if (uz < 1.0) return 0;
  if (uz < 1.0 + half_pow_theta_) return std::min<std::uint64_t>(1, n_ - 1);
  const auto r = static_cast<std::uint64_t>(static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
  return std::min(r, n_ - 1);
}

// ---- Synthetic ---------------------------------------------------------------------

namespace {

class SyntheticWorkload : public Workload {
 public:
  explicit SyntheticWorkload(const SyntheticOptions& o) : o_(o) {
    if (o.keys == 0) throw std::invalid_argument("synthetic workload needs keys >= 1");
    if (o.get_percent > 100) throw std::invalid_argument("get_percent must be 0..100");
    names_.reserve(o.keys);
    for (std::size_t i = 0; i < o.keys; ++i) names_.push_back("key:" + std::to_string(i));
    if (o.distribution == SyntheticOptions::Distribution::Zipf) {
      zipf_ = std::make_unique<ZipfianGenerator>(o.keys, o.zipf_theta);
    }
  }

  std::size_t key_count() const override { return o_.keys; }
  const std::string& key_name(std::uint32_t k) const override { return names_[k]; }

  Op next(std::uint64_t /*seq*/, std::mt19937_64& rng) const override {
    const auto key = static_cast<std::uint32_t>(
        zipf_ ? zipf_->next(rng) : std::uniform_int_distribution<std::uint64_t>(0, o_.keys - 1)(rng));
    const bool get = std::uniform_int_distribution<unsigned>(0, 99)(rng) < o_.get_percent;
    return Op{get ? Op::Type::Get : Op::Type::Set, key, o_.value_size, 0};
  }

  std::vector<Op> preload_ops() const override {
    std::vector<Op> ops;
    ops.reserve(o_.keys);
    for (std::uint32_t k = 0; k < o_.keys; ++k) ops.push_back(Op{Op::Type::Set, k, o_.value_size, 0});
    return ops;
  }

  std::uint32_t max_value_size() const override { return o_.value_size; }
  std::uint32_t refill_size(std::uint32_t) const override { return o_.value_size; }

  std::string describe() const override {
    std::ostringstream s;
    s << (zipf_ ? "zipf(theta=" + std::to_string(o_.zipf_theta).substr(0, 4) + ")" : std::string("uniform"))
      << ", " << o_.keys << " keys, " << o_.value_size << " B values, " << o_.get_percent << "% GET / "
      << (100 - o_.get_percent) << "% SET";
    return s.str();
  }

 private:
  SyntheticOptions o_;
  std::vector<std::string> names_;
  std::unique_ptr<ZipfianGenerator> zipf_;
};

// ---- Trace -------------------------------------------------------------------------

class TraceWorkload : public Workload {
 public:
  TraceWorkload(std::vector<std::string> names, std::vector<Op> ops, std::vector<std::uint32_t> sizes,
                std::string description)
      : names_(std::move(names)), ops_(std::move(ops)), sizes_(std::move(sizes)), desc_(std::move(description)) {}

  std::size_t key_count() const override { return names_.size(); }
  const std::string& key_name(std::uint32_t k) const override { return names_[k]; }
  Op next(std::uint64_t seq, std::mt19937_64& /*rng*/) const override { return ops_[seq % ops_.size()]; }

  // Every key the trace touches, at the largest size the trace shows for it.
  // In production these keys were already cached when the trace started, so
  // replaying into an empty cache would otherwise turn most GETs into misses.
  std::vector<Op> preload_ops() const override {
    std::vector<Op> ops;
    ops.reserve(names_.size());
    for (std::uint32_t k = 0; k < names_.size(); ++k) ops.push_back(Op{Op::Type::Set, k, sizes_[k], 0});
    return ops;
  }

  std::uint32_t max_value_size() const override { return *std::max_element(sizes_.begin(), sizes_.end()); }
  std::uint32_t refill_size(std::uint32_t key) const override { return sizes_[key]; }

  std::string describe() const override { return desc_; }

 private:
  std::vector<std::string> names_;
  std::vector<Op> ops_;
  std::vector<std::uint32_t> sizes_;  // per key: largest value size seen
  std::string desc_;
};

}  // namespace

std::unique_ptr<Workload> make_synthetic_workload(const SyntheticOptions& opts) {
  return std::make_unique<SyntheticWorkload>(opts);
}

std::unique_ptr<Workload> load_trace_workload(std::istream& in, std::size_t max_lines,
                                              std::uint32_t max_value_bytes, const std::string& name,
                                              TraceStats& stats) {
  std::unordered_map<std::string, std::uint32_t> ids;
  std::vector<std::string> names;
  std::vector<std::uint32_t> sizes;
  std::vector<Op> ops;
  std::string line;
  std::vector<std::string> f;

  while (stats.lines < max_lines && std::getline(in, line)) {
    ++stats.lines;
    f.clear();
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) f.push_back(field);
    // timestamp,key,key_size,value_size,client,op,ttl
    if (f.size() != 7 || f[1].empty()) {
      ++stats.skipped;
      continue;
    }
    const std::string& op = f[5];
    Op::Type type;
    if (op == "get" || op == "gets") {
      type = Op::Type::Get;
      ++stats.gets;
    } else if (op == "delete") {
      type = Op::Type::Del;
      ++stats.dels;
    } else if (op == "set" || op == "add" || op == "replace" || op == "cas" || op == "append" ||
               op == "prepend" || op == "incr" || op == "decr") {
      type = Op::Type::Set;
      ++stats.sets;
    } else {
      ++stats.skipped;
      continue;
    }
    std::uint64_t value_size = 0, ttl = 0;
    try {
      value_size = std::stoull(f[3]);
      ttl = std::stoull(f[6]);
    } catch (const std::exception&) {
      ++stats.skipped;
      continue;
    }
    if (value_size > max_value_bytes) {
      value_size = max_value_bytes;
      ++stats.clamped;
    }

    auto [it, inserted] = ids.emplace(f[1], static_cast<std::uint32_t>(names.size()));
    if (inserted) {
      names.push_back(f[1]);
      sizes.push_back(0);
    }
    const std::uint32_t key = it->second;
    sizes[key] = std::max(sizes[key], static_cast<std::uint32_t>(value_size));
    ops.push_back(Op{type, key, static_cast<std::uint32_t>(value_size), static_cast<std::uint32_t>(ttl)});
  }
  if (ops.empty()) throw std::runtime_error("trace contained no usable requests");

  std::ostringstream d;
  d << "trace " << name << ": " << ops.size() << " requests, " << names.size() << " keys ("
    << (100 * stats.gets / ops.size()) << "% GET, " << (100 * stats.sets / ops.size()) << "% SET, "
    << (100 * stats.dels / ops.size()) << "% DEL)";
  return std::make_unique<TraceWorkload>(std::move(names), std::move(ops), std::move(sizes), d.str());
}

std::unique_ptr<Workload> make_workload_from_config(const Config& cfg) {
  const std::string type = cfg.get_string("bench.workload.type");
  if (type == "trace") {
    const std::string path = cfg.get_string("bench.workload.trace_file");
    std::ifstream f(path);
    if (!f) throw ConfigError("cannot open 'bench.workload.trace_file' " + path);
    TraceStats stats;
    return load_trace_workload(f, cfg.get_size("bench.workload.trace_max_requests"),
                               static_cast<std::uint32_t>(cfg.get_bytes("bench.workload.trace_max_value_bytes")),
                               path, stats);
  }
  SyntheticOptions o{};
  if (type == "uniform") {
    o.distribution = SyntheticOptions::Distribution::Uniform;
  } else if (type == "zipf") {
    o.distribution = SyntheticOptions::Distribution::Zipf;
  } else {
    throw ConfigError("'bench.workload.type' must be uniform, zipf or trace, got '" + type + "'");
  }
  o.keys = cfg.get_size("bench.workload.keys");
  o.value_size = static_cast<std::uint32_t>(cfg.get_bytes("bench.workload.value_size"));
  o.get_percent = static_cast<unsigned>(cfg.get_size("bench.workload.get_percent"));
  // Theta is given in hundredths (99 = 0.99) since the config has no decimals.
  o.zipf_theta = static_cast<double>(cfg.get_size("bench.workload.zipf_theta_percent")) / 100.0;
  try {
    return make_synthetic_workload(o);
  } catch (const std::invalid_argument& e) {
    throw ConfigError(std::string("bench.workload: ") + e.what());
  }
}

}  // namespace cachegrid
