#include "cachegrid/Cache.h"

#include <gtest/gtest.h>

using namespace cachegrid;
using namespace std::chrono_literals;

namespace {

// Manually advanced clock so TTL tests are deterministic and instant.
struct FakeClock {
  TimePoint t{};
  NowFn fn() {
    return [this] { return t; };
  }
  void advance(Millis d) { t += d; }
};

constexpr std::size_t kBig = 1 << 20;  // 1 MiB: large enough to never fill up

}  // namespace

// ---- Basic operations --------------------------------------------------------

TEST(CacheBasic, SetThenGetReturnsValue) {
  Cache c(kBig);
  EXPECT_EQ(c.set("k", "v"), SetResult::Ok);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheBasic, GetMissingReturnsNullopt) {
  Cache c(kBig);
  EXPECT_EQ(c.get("nope"), std::nullopt);
}

TEST(CacheBasic, OverwriteReplacesValue) {
  Cache c(kBig);
  c.set("k", "v1");
  c.set("k", "v2");
  EXPECT_EQ(c.get("k"), "v2");
  EXPECT_EQ(c.size(), 1u);
}

TEST(CacheBasic, DeleteRemovesKey) {
  Cache c(kBig);
  c.set("k", "v");
  EXPECT_TRUE(c.del("k"));
  EXPECT_EQ(c.get("k"), std::nullopt);
  EXPECT_FALSE(c.del("k"));
}

TEST(CacheBasic, EmptyAndBinaryValuesRoundTrip) {
  Cache c(kBig);
  const std::string binary("a\0b\r\n", 5);
  c.set("empty", "");
  c.set("bin", binary);
  EXPECT_EQ(c.get("empty"), "");
  EXPECT_EQ(c.get("bin"), binary);
}

// ---- TTL ---------------------------------------------------------------------

TEST(CacheTtl, KeyLiveBeforeExpiry) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(99ms);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheTtl, KeyGoneAtExactExpiry) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(100ms);
  EXPECT_EQ(c.get("k"), std::nullopt);
  EXPECT_EQ(c.size(), 0u) << "lazy expiration should remove the entry";
  EXPECT_EQ(c.used_bytes(), 0u);
}

TEST(CacheTtl, OverwriteWithoutTtlClearsTtl) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("k", "v1", 100ms);
  c.set("k", "v2");
  clk.advance(1h);
  EXPECT_EQ(c.get("k"), "v2");
}

TEST(CacheTtl, OverwriteWithNewTtlResetsDeadline) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("k", "v", 100ms);
  clk.advance(80ms);
  c.set("k", "v", 100ms);
  clk.advance(80ms);
  EXPECT_EQ(c.get("k"), "v");
}

TEST(CacheTtl, NonPositiveTtlRejected) {
  Cache c(kBig);
  EXPECT_EQ(c.set("k", "v", 0ms), SetResult::InvalidTtl);
  EXPECT_EQ(c.set("k", "v", -5ms), SetResult::InvalidTtl);
  EXPECT_EQ(c.size(), 0u);
}

TEST(CacheTtl, DeleteOfExpiredKeyReportsFalse) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("k", "v", 10ms);
  clk.advance(10ms);
  EXPECT_FALSE(c.del("k"));
  EXPECT_EQ(c.size(), 0u);
}

TEST(CacheTtl, PurgeRemovesOnlyExpired) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
  c.set("short", "v", 10ms);
  c.set("long", "v", 1000ms);
  c.set("forever", "v");
  clk.advance(50ms);
  EXPECT_EQ(c.purge_expired(), 1u);
  EXPECT_EQ(c.size(), 2u);
  EXPECT_EQ(c.get("long"), "v");
  EXPECT_EQ(c.get("forever"), "v");
}

// ---- Capacity accounting -----------------------------------------------------

TEST(CacheCapacity, UsedBytesTracksSetOverwriteDelete) {
  Cache c(kBig);
  c.set("key", "12345");
  EXPECT_EQ(c.used_bytes(), Cache::entry_cost("key", "12345"));
  c.set("key", "1");
  EXPECT_EQ(c.used_bytes(), Cache::entry_cost("key", "1"));
  c.del("key");
  EXPECT_EQ(c.used_bytes(), 0u);
}

TEST(CacheCapacity, EntryLargerThanCacheRejected) {
  Cache c(100);
  EXPECT_EQ(c.set("k", std::string(200, 'x')), SetResult::TooLarge);
}

TEST(CacheCapacity, FullCacheRejectsNewKeyAndKeepsOld) {
  const std::size_t one = Cache::entry_cost("a", "v");
  Cache c(one * 2);
  EXPECT_EQ(c.set("a", "v"), SetResult::Ok);
  EXPECT_EQ(c.set("b", "v"), SetResult::Ok);
  EXPECT_EQ(c.set("c", "v"), SetResult::OutOfMemory);
  EXPECT_EQ(c.get("a"), "v");
  EXPECT_EQ(c.get("b"), "v");
  EXPECT_EQ(c.get("c"), std::nullopt);
}

TEST(CacheCapacity, OverwriteThatFitsSucceedsWhenFull) {
  const std::size_t one = Cache::entry_cost("a", "v");
  Cache c(one * 2);
  c.set("a", "v");
  c.set("b", "v");
  EXPECT_EQ(c.set("a", "w"), SetResult::Ok) << "same size overwrite must not need extra room";
}

TEST(CacheCapacity, FailedOverwriteLeavesOldValue) {
  const std::size_t one = Cache::entry_cost("a", "v");
  Cache c(one * 2);
  c.set("a", "v");
  c.set("b", "v");
  EXPECT_EQ(c.set("a", std::string(50, 'x')), SetResult::OutOfMemory);
  EXPECT_EQ(c.get("a"), "v");
}

TEST(CacheCapacity, ExpiredEntriesFreeSpaceForNewWrites) {
  FakeClock clk;
  const std::size_t one = Cache::entry_cost("a", "v");
  Cache c(one, clk.fn());
  c.set("a", "v", 10ms);
  clk.advance(10ms);
  c.purge_expired();
  EXPECT_EQ(c.set("b", "v"), SetResult::Ok);
}

// ---- Stats -------------------------------------------------------------------

TEST(CacheStats, CountsHitsMissesExpirations) {
  FakeClock clk;
  Cache c(kBig, clk.fn());
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
