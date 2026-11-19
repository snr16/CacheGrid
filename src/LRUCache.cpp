#include "cachegrid/LRUCache.h"

#include <utility>

namespace cachegrid {

LRUCache::LRUCache(const LRUOptions& opts, NowFn now)
    : capacity_bytes_(opts.capacity_bytes),
      entry_overhead_bytes_(opts.entry_overhead_bytes),
      now_(std::move(now)),
      policy_(opts.policy) {}

SetResult LRUCache::set(const std::string& key, std::string value, std::optional<Millis> ttl) {
  if (ttl && ttl->count() <= 0) return SetResult::InvalidTtl;

  const std::size_t new_cost = entry_cost(key, value);
  if (new_cost > capacity_bytes_) return SetResult::TooLarge;

  const TimePoint now = now_();
  auto it = map_.find(key);

  // An expired entry is logically absent: drop it first so it does not count
  // against capacity.
  if (it != map_.end() && is_expired(*it->second, now)) {
    erase(it);
    ++stats_.expirations;
    it = map_.end();
  }

  const std::size_t old_cost = (it != map_.end()) ? entry_cost(key, it->second->value) : 0;
  const std::size_t needed = used_bytes_ - old_cost + new_cost;
  if (needed > capacity_bytes_ && policy_ == EvictionPolicy::NoEviction) {
    return SetResult::OutOfMemory;
  }

  std::optional<TimePoint> expires_at;
  if (ttl) expires_at = now + *ttl;

  if (it != map_.end()) {
    it->second->value = std::move(value);
    it->second->expires_at = expires_at;
    touch(it->second);
  } else {
    lru_.push_front(Node{key, std::move(value), expires_at});
    map_.emplace(key, lru_.begin());
  }
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
  ++stats_.hits;
  touch(it->second);
  return it->second->value;
}

bool LRUCache::del(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  const bool was_live = !is_expired(*it->second, now_());
  if (!was_live) ++stats_.expirations;
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

std::list<std::string> LRUCache::keys_by_recency() const {
  std::list<std::string> out;
  for (const auto& n : lru_) out.push_back(n.key);
  return out;
}

bool LRUCache::check_invariants() const {
  if (map_.size() != lru_.size()) return false;
  std::size_t bytes = 0;
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    auto m = map_.find(it->key);
    if (m == map_.end() || m->second != it) return false;
    bytes += entry_cost(it->key, it->value);
  }
  return bytes == used_bytes_ && used_bytes_ <= capacity_bytes_;
}

void LRUCache::erase(Map::iterator it) {
  used_bytes_ -= entry_cost(it->first, it->second->value);
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
