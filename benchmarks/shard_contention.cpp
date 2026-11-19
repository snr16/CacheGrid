// Micro-benchmark: how much does sharding reduce lock contention?
//
// Runs the same GET/SET workload with a varying number of threads and shards
// and prints throughput. Every parameter comes from the config file. Build in
// Release:
//   cmake --preset release && cmake --build --preset release
//   ./build/release/shard_contention config/bench.conf
//
// This measures the in-process cache only, with no networking. End-to-end
// latency benchmarks come in Phase 6.

#include "cachegrid/Cache.h"
#include "cachegrid/Config.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace cachegrid;

namespace {

struct Workload {
  std::size_t keys;
  std::size_t value_size;
  std::size_t ops_per_thread;
  std::size_t write_percent;
};

double run(Cache::Options opts, const Workload& w, std::size_t threads) {
  Cache cache(std::move(opts));
  std::vector<std::string> keys;
  keys.reserve(w.keys);
  for (std::size_t i = 0; i < w.keys; ++i) {
    keys.push_back("key:" + std::to_string(i));
    cache.set(keys.back(), std::string(w.value_size, 'v'));
  }

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      for (std::size_t i = 0; i < w.ops_per_thread; ++i) {
        const std::string& key = keys[rng() % w.keys];
        if (rng() % 100 < w.write_percent) {
          cache.set(key, std::string(w.value_size, 'w'));
        } else {
          cache.get(key);
        }
      }
    });
  }
  for (auto& th : pool) th.join();
  const std::chrono::duration<double> secs = std::chrono::steady_clock::now() - start;
  return static_cast<double>(threads * w.ops_per_thread) / secs.count();
}

std::vector<std::size_t> parse_counts(const Config& cfg, const std::string& key) {
  std::vector<std::size_t> out;
  for (const auto& item : cfg.get_list(key)) {
    if (item == "auto") {
      out.push_back(std::thread::hardware_concurrency());
    } else {
      std::size_t pos = 0;
      unsigned long n = 0;
      try {
        n = std::stoul(item, &pos);
      } catch (const std::exception&) {
        pos = 0;
      }
      if (pos != item.size() || item[0] == '-' || n == 0) {
        throw ConfigError("'" + key + "' has invalid count '" + item + "'");
      }
      out.push_back(n);
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <bench.conf>\n", argv[0]);
    return 2;
  }
  try {
    const Config bench = Config::load_file(argv[1]);
    const Config node = Config::load_file(bench.get_string("bench.cache_config"));

    const Workload w{bench.get_size("bench.shard.keys"), bench.get_size("bench.shard.value_size"),
                     bench.get_size("bench.shard.ops_per_thread"),
                     bench.get_size("bench.shard.write_percent")};
    if (w.keys == 0) throw ConfigError("'bench.shard.keys' must be >= 1");
    if (w.write_percent > 100) throw ConfigError("'bench.shard.write_percent' must be 0..100");

    Cache::Options base = Cache::Options::from_config(node);
    base.capacity_bytes = bench.get_bytes("bench.shard.capacity");

    std::printf("hardware threads: %u | %zu keys, %zuB values, %zu%% writes, %zu ops/thread\n\n",
                std::thread::hardware_concurrency(), w.keys, w.value_size, w.write_percent,
                w.ops_per_thread);
    std::printf("%8s %8s %16s\n", "threads", "shards", "ops/sec (M)");
    for (std::size_t threads : parse_counts(bench, "bench.shard.thread_counts")) {
      for (std::size_t shards : parse_counts(bench, "bench.shard.shard_counts")) {
        Cache::Options opts = base;
        opts.num_shards = shards;
        std::printf("%8zu %8zu %16.2f\n", threads, shards, run(opts, w, threads) / 1e6);
      }
    }
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 1;
  }
  return 0;
}
