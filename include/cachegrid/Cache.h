#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cachegrid/LRUCache.h"

namespace cachegrid {

class Config;

// Thread-safe cache: the public API used by the server.
//
// The key space is split into N shards. Each shard is an independent LRUCache
// protected by its own mutex, and a key always maps to the same shard. Threads
// touching different shards never block each other.
//
// Why not one big mutex or a reader-writer lock? In an LRU cache every GET
// moves the entry to the front of the recency list, so a GET is a write. A
// reader-writer lock would take the exclusive lock on every operation and
// behave just like a plain mutex. Sharding is what actually cuts contention.
//
// Tradeoffs:
//   - LRU is per shard: the evicted entry is the least recently used *in its
//     shard*, not globally. With a reasonable shard count and a good hash this
//     is a close approximation (Memcached and Caffeine make the same choice).
//   - Each shard gets capacity / num_shards bytes, so the largest storable
//     entry is one shard's capacity.
//   - stats()/size()/used_bytes() lock shards one at a time, so the totals are
//     a near-instant snapshot rather than one atomic view.
class Cache {
 public:
  // All tunables come from the config file (config/cachegrid.conf); there are
  // deliberately no built-in defaults.
  struct Options {
    std::size_t capacity_bytes;
    std::size_t num_shards;
    std::size_t entry_overhead_bytes;
    EvictionPolicy policy;
    NowFn now = Clock::now;  // injection point for tests, not a tunable

    // Reads cache.capacity, cache.num_shards, cache.entry_overhead_bytes and
    // cache.eviction_policy. Throws ConfigError on missing/invalid values.
    static Options from_config(const Config& cfg);
  };

  explicit Cache(Options opts);

  SetResult set(const std::string& key, std::string value,
                std::optional<Millis> ttl = std::nullopt);
  std::optional<std::string> get(const std::string& key);
  bool del(const std::string& key);
  std::size_t purge_expired();

  std::size_t size() const;
  std::size_t used_bytes() const;
  std::size_t capacity_bytes() const { return capacity_bytes_; }
  std::size_t num_shards() const { return shards_.size(); }
  CacheStats stats() const;

  // Index of the shard that owns `key`. Exposed for tests.
  std::size_t shard_index(const std::string& key) const;

  // Checks every shard's internal consistency. O(n); for tests.
  bool check_invariants() const;

 private:
  struct Shard {
    Shard(const LRUOptions& o, NowFn now) : lru(o, std::move(now)) {}
    mutable std::mutex mu;
    LRUCache lru;
  };

  Shard& shard_for(const std::string& key) { return *shards_[shard_index(key)]; }

  std::size_t capacity_bytes_;
  // unique_ptr because std::mutex can't be moved, so Shard can't live
  // directly in a growable vector.
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace cachegrid
