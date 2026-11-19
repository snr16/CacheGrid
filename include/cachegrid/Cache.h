#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
  TooLarge,     // single entry larger than the whole cache
  OutOfMemory,  // cache full (Phase 1 policy: "noeviction"; Phase 2 adds LRU)
};

struct CacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t expirations = 0;  // entries removed because their TTL passed
};

// Phase 1: single-threaded, in-memory key/value store with TTL.
//
// Capacity is measured in bytes (key + value + fixed per-entry overhead), not
// entry count, so a few huge values cannot blow past the memory budget.
//
// Expiration is lazy (an expired key is removed when it is next touched) plus
// an explicit purge_expired() sweep that the server will call periodically.
//
// NOT thread-safe. Concurrency arrives in Phase 2.
class Cache {
 public:
  // Rough cost of one entry beyond its key/value bytes: hash node, bucket
  // pointer, std::string headers, expiry timestamp. Approximate on purpose;
  // documented in docs/architecture.md.
  static constexpr std::size_t kEntryOverhead = 64;

  explicit Cache(std::size_t capacity_bytes, NowFn now = Clock::now);

  // Inserts or overwrites. Overwriting without a ttl clears any previous ttl
  // (same semantics as Redis SET). On failure the cache is left unchanged.
  SetResult set(const std::string& key, std::string value,
                std::optional<Millis> ttl = std::nullopt);

  // Returns the value, or nullopt if missing or expired.
  std::optional<std::string> get(const std::string& key);

  // Returns true if a live key was removed.
  bool del(const std::string& key);

  // Removes every expired entry; returns how many were removed. O(n).
  std::size_t purge_expired();

  std::size_t size() const { return map_.size(); }
  std::size_t used_bytes() const { return used_bytes_; }
  std::size_t capacity_bytes() const { return capacity_bytes_; }
  const CacheStats& stats() const { return stats_; }

  static std::size_t entry_cost(const std::string& key, const std::string& value) {
    return key.size() + value.size() + kEntryOverhead;
  }

 private:
  struct Entry {
    std::string value;
    std::optional<TimePoint> expires_at;  // nullopt = never expires
  };
  using Map = std::unordered_map<std::string, Entry>;

  bool is_expired(const Entry& e, TimePoint now) const {
    return e.expires_at && *e.expires_at <= now;
  }
  void erase(Map::iterator it);

  std::size_t capacity_bytes_;
  std::size_t used_bytes_ = 0;
  NowFn now_;
  Map map_;
  CacheStats stats_;
};

}  // namespace cachegrid
