// Single-threaded tests for the LRUCache engine (one shard).
// Thread-safety tests for the sharded Cache live in ConcurrencyTest.cpp.

#include "cachegrid/LRUCache.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <list>
#include <random>
#include <utility>

using namespace cachegrid;
using namespace cachegrid::test;
using namespace std::chrono_literals;
using Keys = std::list<std::string>;

namespace {

// Capacity large enough that these tests never fill the cache.
const std::size_t kBig = cfg_bytes("test.big_capacity");

// Capacity that fits exactly n entries with 1-char key and 1-char value.
std::size_t room_for(std::size_t n) { return n * cost("a", "v"); }

}  // namespace

// ---- Basic operations --------------------------------------------------------

TEST(CacheBasic, SetThenGetReturnsValue) {
  LRUCache c(lru_opts(kBig));
  EXPECT_EQ(c.set("k", "v"), SetResult::Ok);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheBasic, GetMissingReturnsNullopt) {
  LRUCache c(lru_opts(kBig));
  EXPECT_EQ(c.get("nope"), std::nullopt);
}

TEST(CacheBasic, OverwriteReplacesValue) {
  LRUCache c(lru_opts(kBig));
  c.set("k", "v1");
  c.set("k", "v2");
  EXPECT_EQ(c.get("k"), "v2");
  EXPECT_EQ(c.size(), 1u);
}

TEST(CacheBasic, DeleteRemovesKey) {
  LRUCache c(lru_opts(kBig));
  c.set("k", "v");
  EXPECT_TRUE(c.del("k"));
  EXPECT_EQ(c.get("k"), std::nullopt);
  EXPECT_FALSE(c.del("k"));
}

TEST(CacheBasic, EmptyAndBinaryValuesRoundTrip) {
  LRUCache c(lru_opts(kBig));
  const std::string binary("a\0b\r\n", 5);
  c.set("empty", "");
  c.set("bin", binary);
  EXPECT_EQ(c.get("empty"), "");
  EXPECT_EQ(c.get("bin"), binary);
}

// ---- TTL ---------------------------------------------------------------------

TEST(CacheTtl, KeyLiveBeforeExpiry) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(99ms);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheTtl, KeyGoneAtExactExpiry) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(100ms);
  EXPECT_EQ(c.get("k"), std::nullopt);
  EXPECT_EQ(c.size(), 0u) << "lazy expiration should remove the entry";
  EXPECT_EQ(c.used_bytes(), 0u);
}

TEST(CacheTtl, OverwriteWithoutTtlClearsTtl) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("k", "v1", 100ms);
  c.set("k", "v2");
  clk.advance(1h);
  EXPECT_EQ(c.get("k"), "v2");
}

TEST(CacheTtl, OverwriteWithNewTtlResetsDeadline) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(80ms);
  c.set("k", "v", 100ms);
  clk.advance(80ms);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheTtl, NonPositiveTtlRejected) {
  LRUCache c(lru_opts(kBig));
  EXPECT_EQ(c.set("k", "v", 0ms), SetResult::InvalidTtl);
  EXPECT_EQ(c.set("k", "v", -5ms), SetResult::InvalidTtl);
  EXPECT_EQ(c.size(), 0u);
}

TEST(CacheTtl, TtlBeyondClockRangeRejected) {
  // now + ttl would overflow the clock's integer; must be rejected, not wrap.
  LRUCache c(lru_opts(kBig));
  EXPECT_EQ(c.set("k", "v", Millis(std::numeric_limits<Millis::rep>::max())), SetResult::InvalidTtl);
  EXPECT_EQ(c.size(), 0u);
}

TEST(CacheTtl, DeleteOfExpiredKeyReportsFalse) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("k", "v", 10ms);
  clk.advance(10ms);
  EXPECT_FALSE(c.del("k"));
  EXPECT_EQ(c.size(), 0u);
}

TEST(CacheTtl, PurgeRemovesOnlyExpired) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("short", "v", 10ms);
  c.set("long", "v", 1000ms);
  c.set("forever", "v");
  clk.advance(50ms);
  EXPECT_EQ(c.purge_expired(), 1u);
  EXPECT_EQ(c.size(), 2u);
  EXPECT_EQ(c.get("long"), "v");
  EXPECT_EQ(c.get("forever"), "v");
  EXPECT_TRUE(c.check_invariants());
}

// ---- Capacity accounting -----------------------------------------------------

TEST(CacheCapacity, UsedBytesTracksSetOverwriteDelete) {
  LRUCache c(lru_opts(kBig));
  c.set("key", "12345");
  EXPECT_EQ(c.used_bytes(), cost("key", "12345"));
  c.set("key", "1");
  EXPECT_EQ(c.used_bytes(), cost("key", "1"));
  c.del("key");
  EXPECT_EQ(c.used_bytes(), 0u);
}

TEST(CacheCapacity, EntryLargerThanCacheRejected) {
  LRUCache c(lru_opts(100));
  EXPECT_EQ(c.set("k", std::string(200, 'x')), SetResult::TooLarge);
  EXPECT_EQ(c.size(), 0u);
}

// ---- LRU eviction ------------------------------------------------------------

TEST(CacheLru, EvictsLeastRecentlyUsedWhenFull) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  c.set("d", "v");  // full -> evict "a" (oldest)
  EXPECT_EQ(c.get("a"), std::nullopt);
  EXPECT_EQ(c.get("b"), "v");
  EXPECT_EQ(c.get("c"), "v");
  EXPECT_EQ(c.get("d"), "v");
  EXPECT_EQ(c.stats().evictions, 1u);
}

TEST(CacheLru, GetRefreshesRecency) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  c.get("a");       // order now: a, c, b  (b is oldest)
  c.set("d", "v");  // evicts b, not a
  EXPECT_EQ(c.keys_by_recency(), (Keys{"d", "a", "c"}));
}

TEST(CacheLru, OverwriteRefreshesRecency) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  c.set("a", "w");  // a becomes most recent
  c.set("d", "v");  // evicts b
  EXPECT_EQ(c.keys_by_recency(), (Keys{"d", "a", "c"}));
}

TEST(CacheLru, MissDoesNotChangeOrder) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.get("zzz");
  EXPECT_EQ(c.keys_by_recency(), (Keys{"b", "a"}));
}

TEST(CacheLru, DeleteKeepsRemainingOrder) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  c.del("b");
  EXPECT_EQ(c.keys_by_recency(), (Keys{"c", "a"}));
  EXPECT_TRUE(c.check_invariants());
}

TEST(CacheLru, LargeEntryEvictsSeveralSmallOnes) {
  LRUCache c(lru_opts(room_for(4)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  c.set("d", "v");
  // Value of 2 extra entries' worth of bytes needs 3 slots in total.
  const std::string big(1 + 2 * cost("a", "v"), 'x');
  EXPECT_EQ(c.set("e", big), SetResult::Ok);
  EXPECT_EQ(c.keys_by_recency(), (Keys{"e", "d"}));
  EXPECT_EQ(c.stats().evictions, 3u);
  EXPECT_TRUE(c.check_invariants());
}

TEST(CacheLru, GrowingOverwriteEvictsOthersButKeepsItself) {
  LRUCache c(lru_opts(room_for(3)));
  c.set("a", "v");
  c.set("b", "v");
  c.set("c", "v");
  const std::string bigger(1 + cost("a", "v"), 'x');
  EXPECT_EQ(c.set("a", bigger), SetResult::Ok);
  EXPECT_EQ(c.get("a"), bigger);
  EXPECT_EQ(c.keys_by_recency(), (Keys{"a", "c"}));
}

TEST(CacheLru, ExpiredVictimCountsAsExpirationNotEviction) {
  FakeClock clk;
  LRUCache c(lru_opts(room_for(2)), clk.fn());
  c.set("a", "v", 10ms);
  c.set("b", "v");
  clk.advance(10ms);
  c.set("c", "v");  // pushes out "a", which had already expired
  EXPECT_EQ(c.stats().evictions, 0u);
  EXPECT_EQ(c.stats().expirations, 1u);
}

// ---- NoEviction policy (the Phase 1 behaviour, still selectable) --------------

TEST(CacheNoEviction, FullCacheRejectsNewKeyAndKeepsOld) {
  LRUCache c(lru_opts(room_for(2), EvictionPolicy::NoEviction));
  EXPECT_EQ(c.set("a", "v"), SetResult::Ok);
  EXPECT_EQ(c.set("b", "v"), SetResult::Ok);
  EXPECT_EQ(c.set("c", "v"), SetResult::OutOfMemory);
  EXPECT_EQ(c.get("a"), "v");
  EXPECT_EQ(c.get("b"), "v");
  EXPECT_EQ(c.get("c"), std::nullopt);
}

TEST(CacheNoEviction, SameSizeOverwriteSucceedsWhenFull) {
  LRUCache c(lru_opts(room_for(2), EvictionPolicy::NoEviction));
  c.set("a", "v");
  c.set("b", "v");
  EXPECT_EQ(c.set("a", "w"), SetResult::Ok);
}

TEST(CacheNoEviction, FailedOverwriteLeavesOldValue) {
  LRUCache c(lru_opts(room_for(2), EvictionPolicy::NoEviction));
  c.set("a", "v");
  c.set("b", "v");
  EXPECT_EQ(c.set("a", std::string(50, 'x')), SetResult::OutOfMemory);
  EXPECT_EQ(c.get("a"), "v");
}

TEST(CacheNoEviction, ExpiredEntriesFreeSpaceForNewWrites) {
  FakeClock clk;
  LRUCache c(lru_opts(room_for(1), EvictionPolicy::NoEviction), clk.fn());
  c.set("a", "v", 10ms);
  clk.advance(10ms);
  c.purge_expired();
  EXPECT_EQ(c.set("b", "v"), SetResult::Ok);
}

// ---- Stats -------------------------------------------------------------------

TEST(CacheStats, CountsHitsMissesExpirations) {
  FakeClock clk;
  LRUCache c(lru_opts(kBig), clk.fn());
  c.set("a", "v");
  c.set("b", "v", 10ms);
  c.get("a");        // hit
  c.get("missing");  // miss
  clk.advance(10ms);
  c.get("b");        // expired -> miss + expiration
  EXPECT_EQ(c.stats().hits, 1u);
  EXPECT_EQ(c.stats().misses, 2u);
  EXPECT_EQ(c.stats().expirations, 1u);
}

// ---- Randomized check against a reference model -------------------------------
//
// A deliberately simple, obviously correct LRU (linear search over a list) is
// driven with the same random operations as the real cache. After every step
// both must agree on every returned value and on the full recency order.

namespace {
class ModelLru {
 public:
  explicit ModelLru(std::size_t cap) : cap_(cap) {}

  void set(const std::string& k, const std::string& v) {
    remove(k);
    items_.emplace_front(k, v);
    used_ += cost(k, v);
    while (used_ > cap_) {
      used_ -= cost(items_.back().first, items_.back().second);
      items_.pop_back();
    }
  }
  std::optional<std::string> get(const std::string& k) {
    auto it = find(k);
    if (it == items_.end()) return std::nullopt;
    items_.splice(items_.begin(), items_, it);
    return it->second;
  }
  bool del(const std::string& k) { return remove(k); }
  Keys keys() const {
    Keys out;
    for (const auto& kv : items_) out.push_back(kv.first);
    return out;
  }

 private:
  using Items = std::list<std::pair<std::string, std::string>>;
  Items::iterator find(const std::string& k) {
    return std::find_if(items_.begin(), items_.end(), [&](const auto& kv) { return kv.first == k; });
  }
  bool remove(const std::string& k) {
    auto it = find(k);
    if (it == items_.end()) return false;
    used_ -= cost(it->first, it->second);
    items_.erase(it);
    return true;
  }
  std::size_t cap_;
  std::size_t used_ = 0;
  Items items_;
};
}  // namespace

TEST(CacheLru, MatchesReferenceModelOnRandomOperations) {
  // Room for ~10 typical entries, so eviction happens constantly.
  const std::size_t max_len = cfg_size("test.model.max_value_len");
  const std::size_t cap = 10 * cost("key00", std::string(max_len / 2, 'x'));
  LRUCache real(lru_opts(cap));
  ModelLru model(cap);

  std::mt19937 rng(static_cast<unsigned>(cfg_size("test.random_seed")));  // fixed: reproducible
  std::uniform_int_distribution<int> key_dist(0, cfg_int("test.model.key_space") - 1);
  std::uniform_int_distribution<int> op_dist(0, 9);
  std::uniform_int_distribution<std::size_t> len_dist(0, max_len);

  const int steps = cfg_int("test.model.steps");
  for (int step = 0; step < steps; ++step) {
    const std::string key = "key" + std::to_string(key_dist(rng));
    const int op = op_dist(rng);
    if (op < 5) {
      const std::string val(len_dist(rng), static_cast<char>('a' + step % 26));
      ASSERT_EQ(real.set(key, val), SetResult::Ok);
      model.set(key, val);
    } else if (op < 9) {
      ASSERT_EQ(real.get(key), model.get(key)) << "step " << step;
    } else {
      ASSERT_EQ(real.del(key), model.del(key)) << "step " << step;
    }
    ASSERT_EQ(real.keys_by_recency(), model.keys()) << "step " << step;
  }
  EXPECT_TRUE(real.check_invariants());
}
