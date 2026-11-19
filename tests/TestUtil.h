#pragma once

// Shared test fixtures.
//
// Every tunable test parameter (sizes, counts, timeouts, threads, tolerances)
// comes from config/test.conf, a file separate from the node/proxy configs, so
// retuning production can never break the tests. ConfigTest.cpp separately
// checks that the shipped production configs are valid.

#include "cachegrid/Cache.h"
#include "cachegrid/Config.h"
#include "cachegrid/LRUCache.h"
#include "cachegrid/Net.h"

#include <string>

namespace cachegrid::test {

// config/test.conf, loaded once.
inline const Config& cfg() {
  static const Config c = Config::load_file(CACHEGRID_SOURCE_DIR "/config/test.conf");
  return c;
}
inline std::size_t cfg_size(const std::string& key) { return cfg().get_size(key); }
inline std::size_t cfg_bytes(const std::string& key) { return cfg().get_bytes(key); }
inline int cfg_int(const std::string& key) { return static_cast<int>(cfg().get_size(key)); }
inline Millis cfg_ms(const std::string& key) { return Millis(cfg().get_size(key)); }

inline std::size_t overhead() { return cfg_bytes("test.entry_overhead_bytes"); }

inline std::size_t cost(const std::string& key, const std::string& value) {
  return key.size() + value.size() + overhead();
}

inline LRUOptions lru_opts(std::size_t capacity, EvictionPolicy p = EvictionPolicy::Lru) {
  return LRUOptions{capacity, overhead(), p};
}

inline Cache::Options cache_opts(std::size_t capacity, std::size_t shards) {
  Cache::Options o{};
  o.capacity_bytes = capacity;
  o.num_shards = shards;
  o.entry_overhead_bytes = overhead();
  o.policy = EvictionPolicy::Lru;
  return o;
}

// Listener settings for in-process servers/proxies, from `test.listener.*`.
inline ListenerOptions test_listener_opts() {
  return ListenerOptions::from_config(cfg(), "test.listener", 0);
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
