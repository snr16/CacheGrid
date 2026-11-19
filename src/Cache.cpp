#include "cachegrid/Cache.h"

#include <functional>
#include <stdexcept>

#include "cachegrid/Config.h"

namespace cachegrid {

Cache::Options Cache::Options::from_config(const Config& cfg) {
  Options o{};
  o.capacity_bytes = cfg.get_bytes("cache.capacity");
  o.num_shards = cfg.get_size("cache.num_shards");
  o.entry_overhead_bytes = cfg.get_bytes("cache.entry_overhead_bytes");

  const std::string policy = cfg.get_string("cache.eviction_policy");
  if (policy == "lru") {
    o.policy = EvictionPolicy::Lru;
  } else if (policy == "noeviction") {
    o.policy = EvictionPolicy::NoEviction;
  } else {
    throw ConfigError("'cache.eviction_policy' must be 'lru' or 'noeviction', got '" + policy + "'");
  }
  if (o.num_shards == 0) throw ConfigError("'cache.num_shards' must be >= 1");
  if (o.capacity_bytes < o.num_shards) {
    throw ConfigError("'cache.capacity' must be at least one byte per shard");
  }
  return o;
}

Cache::Cache(Options opts) : capacity_bytes_(opts.capacity_bytes) {
  if (opts.num_shards == 0) throw std::invalid_argument("num_shards must be >= 1");
  const LRUOptions shard_opts{opts.capacity_bytes / opts.num_shards, opts.entry_overhead_bytes,
                              opts.policy};
  shards_.reserve(opts.num_shards);
  for (std::size_t i = 0; i < opts.num_shards; ++i) {
    shards_.push_back(std::make_unique<Shard>(shard_opts, opts.now));
  }
}

std::size_t Cache::shard_index(const std::string& key) const {
  // std::hash is fine here. The cluster-level hash ring (Phase 4) must use a
  // *different* hash function: if both used the same one, all keys that land
  // on one node would share hash bits and crowd into a few shards.
  return std::hash<std::string>{}(key) % shards_.size();
}

SetResult Cache::set(const std::string& key, std::string value, std::optional<Millis> ttl) {
  Shard& s = shard_for(key);
  std::lock_guard<std::mutex> lock(s.mu);
  return s.lru.set(key, std::move(value), ttl);
}

std::optional<std::string> Cache::get(const std::string& key) {
  Shard& s = shard_for(key);
  std::lock_guard<std::mutex> lock(s.mu);
  // Returns a copy, made while the lock is held. Returning a reference or a
  // string_view would let another thread overwrite or evict the value while
  // the caller is still reading it.
  return s.lru.get(key);
}

bool Cache::del(const std::string& key) {
  Shard& s = shard_for(key);
  std::lock_guard<std::mutex> lock(s.mu);
  return s.lru.del(key);
}

std::size_t Cache::purge_expired() {
  // One shard at a time, so a sweep never stalls the whole cache.
  std::size_t removed = 0;
  for (auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s->mu);
    removed += s->lru.purge_expired();
  }
  return removed;
}

std::size_t Cache::size() const {
  std::size_t total = 0;
  for (const auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s->mu);
    total += s->lru.size();
  }
  return total;
}

std::size_t Cache::used_bytes() const {
  std::size_t total = 0;
  for (const auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s->mu);
    total += s->lru.used_bytes();
  }
  return total;
}

CacheStats Cache::stats() const {
  CacheStats total;
  for (const auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s->mu);
    total += s->lru.stats();
  }
  return total;
}

bool Cache::check_invariants() const {
  for (const auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s->mu);
    if (!s->lru.check_invariants()) return false;
  }
  return true;
}

}  // namespace cachegrid
