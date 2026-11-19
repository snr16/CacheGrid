// Thread-safety tests for the sharded Cache.
// Run these under ThreadSanitizer:  cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

#include "cachegrid/Cache.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace cachegrid;
using namespace cachegrid::test;

namespace {

constexpr int kThreads = 8;

// Starts n threads running fn(thread_id) and waits for all of them.
template <typename Fn>
void run_threads(int n, Fn fn) {
  std::vector<std::thread> threads;
  threads.reserve(n);
  for (int t = 0; t < n; ++t) threads.emplace_back(fn, t);
  for (auto& th : threads) th.join();
}

}  // namespace

// ---- Sharding basics ---------------------------------------------------------

TEST(Sharding, ZeroShardsRejected) {
  EXPECT_THROW(Cache(cache_opts(1024, 0)), std::invalid_argument);
}

TEST(Sharding, SameKeyAlwaysSameShard) {
  Cache c(cache_opts(1 << 20, 16));
  EXPECT_EQ(c.shard_index("user:42"), c.shard_index("user:42"));
}

TEST(Sharding, KeysSpreadAcrossShards) {
  Cache c(cache_opts(1 << 20, 16));
  std::set<std::size_t> used;
  for (int i = 0; i < 1000; ++i) used.insert(c.shard_index("key" + std::to_string(i)));
  EXPECT_EQ(used.size(), 16u) << "1000 keys should touch every one of 16 shards";
}

TEST(Sharding, AggregatesAcrossShards) {
  Cache c(cache_opts(1 << 20, 4));
  for (int i = 0; i < 100; ++i) c.set("k" + std::to_string(i), "v");
  EXPECT_EQ(c.size(), 100u);
  // k0..k9 have 2-char keys, k10..k99 have 3-char keys.
  EXPECT_EQ(c.used_bytes(),
            10 * cost("k0", "v") + 90 * cost("k10", "v"));
  EXPECT_TRUE(c.check_invariants());
}

// ---- Concurrent correctness --------------------------------------------------

// Each thread owns its own keys. Nothing may be lost or mixed up.
TEST(Concurrency, DisjointWritersLoseNothing) {
  Cache c(cache_opts(64 << 20, 16));
  constexpr int kPerThread = 5000;

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kPerThread; ++i) {
      const std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
      c.set(key, key);
    }
  });

  EXPECT_EQ(c.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; i += 97) {
      const std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
      ASSERT_EQ(c.get(key), key);
    }
  }
  EXPECT_TRUE(c.check_invariants());
}

// All threads fight over a small set of hot keys with mixed operations. Every
// value a reader sees must be one some writer actually wrote ("no torn
// values"), and the structure must be intact at the end.
TEST(Concurrency, HotKeysMixedOperationsStayConsistent) {
  Cache c(cache_opts(64 << 20, 4));
  constexpr int kOps = 20000;
  constexpr int kHotKeys = 16;
  std::atomic<int> bad_values{0};

  run_threads(kThreads, [&](int t) {
    std::mt19937 rng(static_cast<unsigned>(t));
    for (int i = 0; i < kOps; ++i) {
      const int k = static_cast<int>(rng() % kHotKeys);
      const std::string key = "hot" + std::to_string(k);
      switch (rng() % 3) {
        case 0:
          // Value encodes its key, so a reader can verify it wasn't mixed up.
          c.set(key, key + "|" + std::to_string(t));
          break;
        case 1: {
          auto v = c.get(key);
          if (v && v->rfind(key + "|", 0) != 0) bad_values.fetch_add(1);
          break;
        }
        default:
          c.del(key);
      }
    }
  });

  EXPECT_EQ(bad_values.load(), 0);
  EXPECT_LE(c.size(), static_cast<std::size_t>(kHotKeys));
  EXPECT_TRUE(c.check_invariants());
}

// A small cache under heavy write load: eviction runs constantly from many
// threads, and the memory limit must hold throughout.
TEST(Concurrency, EvictionUnderLoadRespectsCapacity) {
  const std::size_t cap = 4 * 200 * cost("t0:000000", std::string(32, 'x'));
  Cache c(cache_opts(cap, 4));

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < 10000; ++i) {
      c.set("t" + std::to_string(t) + ":" + std::to_string(i), std::string(32, 'x'));
      c.get("t" + std::to_string(t) + ":" + std::to_string(i / 2));
    }
  });

  EXPECT_LE(c.used_bytes(), cap);
  EXPECT_GT(c.stats().evictions, 0u);
  EXPECT_TRUE(c.check_invariants());
}

// Every get() is counted exactly once as a hit or a miss, even under contention.
TEST(Concurrency, StatsCountEveryOperation) {
  Cache c(cache_opts(64 << 20, 8));
  constexpr int kGets = 10000;
  for (int i = 0; i < 100; ++i) c.set("k" + std::to_string(i), "v");

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kGets; ++i) c.get("k" + std::to_string((i + t) % 200));  // ~half miss
  });

  const CacheStats s = c.stats();
  EXPECT_EQ(s.hits + s.misses, static_cast<std::uint64_t>(kThreads * kGets));
  EXPECT_EQ(s.hits, s.misses);
}

// Background sweeper running while other threads read and write, which is how
// the server will use purge_expired() in Phase 3.
TEST(Concurrency, PurgeWhileReadingAndWriting) {
  Cache c(cache_opts(64 << 20, 8));
  std::atomic<bool> stop{false};

  std::thread sweeper([&] {
    while (!stop.load()) c.purge_expired();
  });
  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < 5000; ++i) {
      const std::string key = "k" + std::to_string(i % 500);
      c.set(key, "v", Millis(1 + (i + t) % 3));
      c.get(key);
    }
  });
  stop.store(true);
  sweeper.join();

  EXPECT_TRUE(c.check_invariants());
}
