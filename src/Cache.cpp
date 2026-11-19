#include "cachegrid/Cache.h"

#include <utility>

namespace cachegrid {

Cache::Cache(std::size_t capacity_bytes, NowFn now)
    : capacity_bytes_(capacity_bytes), now_(std::move(now)) {}

SetResult Cache::set(const std::string& key, std::string value, std::optional<Millis> ttl) {
  if (ttl && ttl->count() <= 0) return SetResult::InvalidTtl;

  const std::size_t new_cost = entry_cost(key, value);
  if (new_cost > capacity_bytes_) return SetResult::TooLarge;

  const TimePoint now = now_();
  auto it = map_.find(key);

  // An expired entry is logically absent: drop it before doing the math so it
  // does not count against capacity.
  if (it != map_.end() && is_expired(it->second, now)) {
    erase(it);
    ++stats_.expirations;
    it = map_.end();
  }

  const std::size_t old_cost = (it != map_.end()) ? entry_cost(key, it->second.value) : 0;
  if (used_bytes_ - old_cost + new_cost > capacity_bytes_) return SetResult::OutOfMemory;

  std::optional<TimePoint> expires_at;
  if (ttl) expires_at = now + *ttl;

  if (it != map_.end()) {
    it->second.value = std::move(value);
    it->second.expires_at = expires_at;
  } else {
    map_.emplace(key, Entry{std::move(value), expires_at});
  }
  used_bytes_ = used_bytes_ - old_cost + new_cost;
  return SetResult::Ok;
}

std::optional<std::string> Cache::get(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) {
    ++stats_.misses;
    return std::nullopt;
  }
  if (is_expired(it->second, now_())) {
    erase(it);
    ++stats_.expirations;
    ++stats_.misses;
    return std::nullopt;
  }
  ++stats_.hits;
  return it->second.value;
}

bool Cache::del(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  const bool was_live = !is_expired(it->second, now_());
  if (!was_live) ++stats_.expirations;
  erase(it);
  return was_live;
}

std::size_t Cache::purge_expired() {
  const TimePoint now = now_();
  std::size_t removed = 0;
  for (auto it = map_.begin(); it != map_.end();) {
    if (is_expired(it->second, now)) {
      used_bytes_ -= entry_cost(it->first, it->second.value);
      it = map_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  stats_.expirations += removed;
  return removed;
}

void Cache::erase(Map::iterator it) {
  used_bytes_ -= entry_cost(it->first, it->second.value);
  map_.erase(it);
}

}  // namespace cachegrid
