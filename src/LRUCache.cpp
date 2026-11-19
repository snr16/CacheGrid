#include "cachegrid/LRUCache.h"

#include <utility>

namespace cachegrid {

LRUCache::LRUCache(const LRUOptions& opts, NowFn now)
    : capacity_bytes_(opts.capacity_bytes),
      entry_overhead_bytes_(opts.entry_overhead_bytes),
      now_(std::move(now)),
      policy_(opts.policy) {}

SetResult LRUCache::set(const std::string& key, std::string value, std::optional<Millis> ttl) {
  return write(key, std::move(value), ttl, 0, /*check_version=*/false, /*tombstone=*/false);
}

SetResult LRUCache::set_versioned(const std::string& key, std::string value, std::optional<Millis> ttl,
                                  std::uint64_t version) {
  return write(key, std::move(value), ttl, version, /*check_version=*/true, /*tombstone=*/false);
}

DelResult LRUCache::del_versioned(const std::string& key, std::uint64_t version, Millis tombstone_ttl) {
  bool removed_live = false;
  auto it = map_.find(key);
  if (it != map_.end() && !it->second->tombstone && !is_expired(*it->second, now_())) {
    removed_live = true;
  }
  const SetResult r = write(key, std::string(), tombstone_ttl, version, /*check_version=*/true,
                            /*tombstone=*/true);
  return DelResult{r, r == SetResult::Ok && removed_live};
}

SetResult LRUCache::write(const std::string& key, std::string value, std::optional<Millis> ttl,
                          std::uint64_t version, bool check_version, bool tombstone) {
  if (ttl && ttl->count() <= 0) return SetResult::InvalidTtl;

  const std::size_t new_cost = entry_cost(key, value);
  if (new_cost > capacity_bytes_) return SetResult::TooLarge;

  const TimePoint now = now_();

  // Reject a TTL so far in the future that `now + ttl` would overflow the
  // clock's integer (undefined behaviour). Compared in milliseconds, because
  // converting a huge ttl to the clock's nanoseconds would itself overflow.
  if (ttl && ttl->count() > std::chrono::duration_cast<Millis>(TimePoint::max() - now).count()) {
    return SetResult::InvalidTtl;
  }

  auto it = map_.find(key);

  // An expired entry is logically absent: drop it first so it does not count
  // against capacity (or block a versioned write).
  if (it != map_.end() && is_expired(*it->second, now)) {
    erase(it);
    ++stats_.expirations;
    it = map_.end();
  }

  // Last writer wins: an equal or older version changes nothing. (Equal
  // matters too: the same write delivered twice must be harmless.)
  if (check_version && it != map_.end() && it->second->version >= version) {
    return SetResult::Stale;
  }

  const std::size_t old_cost = (it != map_.end()) ? entry_cost(key, it->second->value) : 0;
  const std::size_t needed = used_bytes_ - old_cost + new_cost;
  if (needed > capacity_bytes_ && policy_ == EvictionPolicy::NoEviction) {
    return SetResult::OutOfMemory;
  }

  std::optional<TimePoint> expires_at;
  if (ttl) expires_at = now + *ttl;

  if (it != map_.end()) {
    Node& n = *it->second;
    if (n.tombstone) --tombstones_;
    n.value = std::move(value);
    n.expires_at = expires_at;
    n.version = version;
    n.tombstone = tombstone;
    touch(it->second);
  } else {
    lru_.push_front(Node{key, std::move(value), expires_at, version, tombstone});
    map_.emplace(key, lru_.begin());
  }
  if (tombstone) ++tombstones_;
  used_bytes_ = needed;

  // The new entry is at the front and new_cost <= capacity, so evicting from
  // the back always terminates before reaching it.
  evict_until_fits(now);
  return SetResult::Ok;
}

std::optional<std::string> LRUCache::get(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) {
    ++stats_.misses;
    return std::nullopt;
  }
  if (is_expired(*it->second, now_())) {
    erase(it);
    ++stats_.expirations;
    ++stats_.misses;
    return std::nullopt;
  }
  if (it->second->tombstone) {  // deleted: reads as missing
    ++stats_.misses;
    return std::nullopt;
  }
  ++stats_.hits;
  touch(it->second);
  return it->second->value;
}

bool LRUCache::del(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  const bool expired = is_expired(*it->second, now_());
  if (expired) ++stats_.expirations;
  const bool was_live = !expired && !it->second->tombstone;
  erase(it);
  return was_live;
}

std::size_t LRUCache::purge_expired() {
  const TimePoint now = now_();
  std::size_t removed = 0;
  for (auto node = lru_.begin(); node != lru_.end();) {
    auto next = std::next(node);
    if (is_expired(*node, now)) {
      erase(map_.find(node->key));
      ++removed;
    }
    node = next;
  }
  stats_.expirations += removed;
  return removed;
}

std::vector<ExportedEntry> LRUCache::export_entries() const {
  const TimePoint now = now_();
  std::vector<ExportedEntry> out;
  out.reserve(lru_.size());
  for (const Node& n : lru_) {
    if (is_expired(n, now)) continue;
    std::optional<Millis> ttl;
    if (n.expires_at) {
      ttl = std::chrono::duration_cast<Millis>(*n.expires_at - now);
      // Less than 1ms left rounds to 0, which isn't a valid TTL; skip it, it
      // is about to expire anyway.
      if (ttl->count() <= 0) continue;
    }
    out.push_back(ExportedEntry{n.key, n.value, n.version, n.tombstone, ttl});
  }
  return out;
}

void LRUCache::clear() {
  lru_.clear();
  map_.clear();
  used_bytes_ = 0;
  tombstones_ = 0;
}

std::list<std::string> LRUCache::keys_by_recency() const {
  std::list<std::string> out;
  for (const auto& n : lru_) out.push_back(n.key);
  return out;
}

bool LRUCache::check_invariants() const {
  if (map_.size() != lru_.size()) return false;
  std::size_t bytes = 0;
  std::size_t tombs = 0;
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    auto m = map_.find(it->key);
    if (m == map_.end() || m->second != it) return false;
    bytes += entry_cost(it->key, it->value);
    if (it->tombstone) ++tombs;
  }
  return bytes == used_bytes_ && used_bytes_ <= capacity_bytes_ && tombs == tombstones_;
}

void LRUCache::erase(Map::iterator it) {
  used_bytes_ -= entry_cost(it->first, it->second->value);
  if (it->second->tombstone) --tombstones_;
  lru_.erase(it->second);
  map_.erase(it);
}

void LRUCache::evict_until_fits(TimePoint now) {
  while (used_bytes_ > capacity_bytes_) {
    const Node& victim = lru_.back();
    if (is_expired(victim, now)) {
      ++stats_.expirations;
    } else {
      ++stats_.evictions;
    }
    erase(map_.find(victim.key));
  }
}

}  // namespace cachegrid
