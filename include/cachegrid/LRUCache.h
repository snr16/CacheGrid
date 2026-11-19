#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
  Stale,        // versioned write ignored: the cache already holds a newer (or equal) version
};

// Result of a versioned delete.
struct DelResult {
  SetResult status;   // Ok (tombstone written) or Stale (a newer version exists)
  bool removed_live;  // a live value was removed by this delete
};

// One entry as copied between nodes during recovery (see Cache::export_shard).
struct ExportedEntry {
  std::string key;
  std::string value;
  std::uint64_t version;
  bool tombstone;
  std::optional<Millis> ttl_remaining;  // nullopt = never expires
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
// Versions and tombstones (used for replication, Phase 5):
//   - Every entry carries a version number. Plain set() writes version 0.
//   - set_versioned() only applies if its version is NEWER than the stored
//     one, so replaying an old write can never overwrite a newer value
//     ("last writer wins" by version).
//   - del_versioned() leaves a *tombstone*: an entry that means "deleted at
//     version V". It reads as missing, but still blocks older writes, so a
//     delayed or re-copied old value can't bring a deleted key back to life.
//     Tombstones expire after a TTL so they don't pile up forever.
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

  // Returns true if a live key was removed. Removes tombstones too (an
  // unconditional delete, used for local administration and trimming).
  bool del(const std::string& key);

  // Applies only if `version` is newer than the stored entry's version
  // (live or tombstone). Otherwise returns Stale and changes nothing.
  SetResult set_versioned(const std::string& key, std::string value, std::optional<Millis> ttl,
                          std::uint64_t version);

  // Replaces the entry with a tombstone at `version` (kept for
  // `tombstone_ttl`), unless a newer version is stored.
  DelResult del_versioned(const std::string& key, std::uint64_t version, Millis tombstone_ttl);

  // All non-expired entries (live and tombstones), with remaining TTLs.
  std::vector<ExportedEntry> export_entries() const;

  // Removes everything.
  void clear();

  // Removes every expired entry; returns how many were removed. O(n).
  std::size_t purge_expired();

  // Live keys (tombstones excluded).
  std::size_t size() const { return map_.size() - tombstones_; }
  std::size_t tombstone_count() const { return tombstones_; }
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
    std::uint64_t version = 0;
    bool tombstone = false;
  };
  using List = std::list<Node>;
  using Map = std::unordered_map<std::string, List::iterator>;

  bool is_expired(const Node& n, TimePoint now) const {
    return n.expires_at && *n.expires_at <= now;
  }
  void touch(List::iterator it) { lru_.splice(lru_.begin(), lru_, it); }
  void erase(Map::iterator it);
  // Shared by set(), set_versioned() and del_versioned().
  SetResult write(const std::string& key, std::string value, std::optional<Millis> ttl,
                  std::uint64_t version, bool check_version, bool tombstone);
  void evict_until_fits(TimePoint now);

  std::size_t capacity_bytes_;
  std::size_t entry_overhead_bytes_;
  std::size_t used_bytes_ = 0;
  std::size_t tombstones_ = 0;
  NowFn now_;
  EvictionPolicy policy_;
  List lru_;  // front = most recently used
  Map map_;
  CacheStats stats_;
};

}  // namespace cachegrid
