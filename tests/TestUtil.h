#pragma once

// Shared test fixtures.
//
// These constants are deliberately independent of config/cachegrid.conf: unit
// tests need fixed, known inputs, so changing production tuning must never
// break them. ConfigTest.cpp separately checks that the shipped config file
// parses and is valid.

#include "cachegrid/Cache.h"
#include "cachegrid/LRUCache.h"

#include <string>

namespace cachegrid::test {

constexpr std::size_t kOverhead = 96;

inline std::size_t cost(const std::string& key, const std::string& value) {
  return key.size() + value.size() + kOverhead;
}

inline LRUOptions lru_opts(std::size_t capacity, EvictionPolicy p = EvictionPolicy::Lru) {
  return LRUOptions{capacity, kOverhead, p};
}

inline Cache::Options cache_opts(std::size_t capacity, std::size_t shards) {
  Cache::Options o{};
  o.capacity_bytes = capacity;
  o.num_shards = shards;
  o.entry_overhead_bytes = kOverhead;
  o.policy = EvictionPolicy::Lru;
  return o;
}

// Manually advanced clock so TTL tests are deterministic and instant.
struct FakeClock {
  TimePoint t{};
  NowFn fn() {
    return [this] { return t; };
  }
  void advance(Millis d) { t += d; }
};

}  // namespace cachegrid::test
