#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace cachegrid {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;

// Injectable time source. Production uses the steady clock; tests pass a fake
// clock so TTL behaviour can be verified without sleeping.
using NowFn = std::function<TimePoint()>;

enum class SetResult {
  Ok,
  InvalidTtl,   // ttl was provided but <= 0
  TooLarge,     // single entry larger than the whole cache (or shard)
  OutOfMemory,  // cache full and policy is NoEviction
};

enum class EvictionPolicy {
  Lru,         // evict least recently used entries to make room (default)
  NoEviction,  // reject writes when full (Redis "noeviction")
};

struct CacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t expirations = 0;  // entries removed because their TTL passed
  std::uint64_t evictions = 0;    // live entries removed to make room

  CacheStats& operator+=(const CacheStats& o) {
    hits += o.hits;
    misses += o.misses;
    expirations += o.expirations;
    evictions += o.evictions;
    return *this;
  }
};

struct LRUOptions {
  std::size_t capacity_bytes;
  // Estimated per-entry cost beyond key/value bytes: list node (2 pointers),
  // hash node + bucket slot, two std::string headers, expiry timestamp, and
  // the duplicated key stored in the map. Set in config.
  std::size_t entry_overhead_bytes;
  EvictionPolicy policy;
};

// Single-shard key/value store with TTL and LRU eviction.
//
// Data structure: hash map (key -> list iterator) + doubly linked list ordered
// by recency (front = most recently used, back = least). Every operation is
// O(1) on average:
//   - lookup:          hash map
//   - "mark as used":  std::list::splice moves the node to the front, O(1)
//   - evict:           pop the back of the list
//
// Capacity is measured in bytes (key + value + fixed per-entry overhead).
//
// NOT thread-safe. The thread-safe wrapper is cachegrid::Cache, which holds
// several LRUCache shards, each behind its own mutex.
class LRUCache {
 public:
  explicit LRUCache(const LRUOptions& opts, NowFn now = Clock::now);

  // Inserts or overwrites and marks the key most recently used. Overwriting
  // without a ttl clears any previous ttl (Redis SET semantics). On failure
  // the cache is left unchanged.
  SetResult set(const std::string& key, std::string value,
                std::optional<Millis> ttl = std::nullopt);

  // Returns the value (and marks it most recently used), or nullopt if
  // missing or expired.
  std::optional<std::string> get(const std::string& key);

  // Returns true if a live key was removed.
  bool del(const std::string& key);

  // Removes every expired entry; returns how many were removed. O(n).
  std::size_t purge_expired();

  std::size_t size() const { return map_.size(); }
  std::size_t used_bytes() const { return used_bytes_; }
  std::size_t capacity_bytes() const { return capacity_bytes_; }
  const CacheStats& stats() const { return stats_; }

  // Keys from most to least recently used. O(n); for tests and debugging.
  std::list<std::string> keys_by_recency() const;

  // Verifies internal consistency (map and list agree, byte count is exact,
  // capacity respected). O(n); for tests.
  bool check_invariants() const;

  std::size_t entry_cost(const std::string& key, const std::string& value) const {
    return key.size() + value.size() + entry_overhead_bytes_;
  }

 private:
  struct Node {
    std::string key;
    std::string value;
    std::optional<TimePoint> expires_at;  // nullopt = never expires
  };
  using List = std::list<Node>;
  using Map = std::unordered_map<std::string, List::iterator>;

  bool is_expired(const Node& n, TimePoint now) const {
    return n.expires_at && *n.expires_at <= now;
  }
  void touch(List::iterator it) { lru_.splice(lru_.begin(), lru_, it); }
  void erase(Map::iterator it);
  void evict_until_fits(TimePoint now);

  std::size_t capacity_bytes_;
  std::size_t entry_overhead_bytes_;
  std::size_t used_bytes_ = 0;
  NowFn now_;
  EvictionPolicy policy_;
  List lru_;  // front = most recently used
  Map map_;
  CacheStats stats_;
};

}  // namespace cachegrid
