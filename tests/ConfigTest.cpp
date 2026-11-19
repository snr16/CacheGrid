#include "cachegrid/Cache.h"
#include "cachegrid/CacheServer.h"
#include "cachegrid/Config.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace cachegrid;

// ---- Parsing -----------------------------------------------------------------

TEST(Config, ParsesKeysValuesCommentsAndWhitespace) {
  const Config c = Config::parse(
      "# comment\n"
      "\n"
      "  a.b   =   hello world  \n"
      "n=42\r\n");
  EXPECT_EQ(c.get_string("a.b"), "hello world");
  EXPECT_EQ(c.get_int("n"), 42);
  EXPECT_FALSE(c.has("comment"));
}

TEST(Config, LineWithoutEqualsReportsLineNumber) {
  try {
    Config::parse("a=1\nbroken line\n", "test.conf");
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("test.conf:2"), std::string::npos) << e.what();
  }
}

TEST(Config, DuplicateKeyRejected) {
  EXPECT_THROW(Config::parse("a=1\na=2\n"), ConfigError);
}

TEST(Config, EmptyKeyRejected) {
  EXPECT_THROW(Config::parse("=1\n"), ConfigError);
}

TEST(Config, MissingFileRejected) {
  EXPECT_THROW(Config::load_file("/nonexistent/cachegrid.conf"), ConfigError);
}

// ---- Getters -----------------------------------------------------------------

TEST(Config, MissingKeyErrorNamesTheKey) {
  const Config c = Config::parse("");
  try {
    c.get_string("cache.capacity");
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("cache.capacity"), std::string::npos) << e.what();
  }
}

TEST(Config, EmptyValueRejected) {
  EXPECT_THROW(Config::parse("a=\n").get_string("a"), ConfigError);
}

TEST(Config, IntRejectsJunk) {
  const Config c = Config::parse("a=12abc\nb=\nc=-7\nd=99999999999999999999\n");
  EXPECT_THROW(c.get_int("a"), ConfigError);
  EXPECT_EQ(c.get_int("c"), -7);
  EXPECT_THROW(c.get_int("d"), ConfigError);
}

TEST(Config, SizeRejectsNegative) {
  EXPECT_THROW(Config::parse("a=-1\n").get_size("a"), ConfigError);
}

TEST(Config, BytesUnderstandsUnits) {
  const Config c = Config::parse("a=512\nb=512B\nc=64KB\nd=64 mb\ne=2GB\n");
  EXPECT_EQ(c.get_bytes("a"), 512u);
  EXPECT_EQ(c.get_bytes("b"), 512u);
  EXPECT_EQ(c.get_bytes("c"), 64u * 1024);
  EXPECT_EQ(c.get_bytes("d"), 64u * 1024 * 1024);
  EXPECT_EQ(c.get_bytes("e"), 2ull * 1024 * 1024 * 1024);
}

TEST(Config, BytesRejectsBadInput) {
  const Config c = Config::parse("a=64TB\nb=MB\nc=-5MB\n");
  EXPECT_THROW(c.get_bytes("a"), ConfigError);
  EXPECT_THROW(c.get_bytes("b"), ConfigError);
  EXPECT_THROW(c.get_bytes("c"), ConfigError);
}

TEST(Config, ListSplitsAndTrims) {
  const Config c = Config::parse("a = 1, 4 ,16\nb=1,,2\nc=1,\n");
  EXPECT_EQ(c.get_list("a"), (std::vector<std::string>{"1", "4", "16"}));
  EXPECT_THROW(c.get_list("b"), ConfigError);
  EXPECT_THROW(c.get_list("c"), ConfigError);
}

// ---- Cache options from config -----------------------------------------------

TEST(CacheOptionsFromConfig, ReadsAllFields) {
  const Config c = Config::parse(
      "cache.capacity = 1MB\n"
      "cache.num_shards = 8\n"
      "cache.eviction_policy = noeviction\n"
      "cache.entry_overhead_bytes = 100\n");
  const auto o = Cache::Options::from_config(c);
  EXPECT_EQ(o.capacity_bytes, 1u << 20);
  EXPECT_EQ(o.num_shards, 8u);
  EXPECT_EQ(o.policy, EvictionPolicy::NoEviction);
  EXPECT_EQ(o.entry_overhead_bytes, 100u);
}

TEST(CacheOptionsFromConfig, RejectsBadPolicyAndZeroShards) {
  const std::string base = "cache.capacity=1MB\ncache.entry_overhead_bytes=96\n";
  EXPECT_THROW(Cache::Options::from_config(
                   Config::parse(base + "cache.num_shards=8\ncache.eviction_policy=fifo\n")),
               ConfigError);
  EXPECT_THROW(Cache::Options::from_config(
                   Config::parse(base + "cache.num_shards=0\ncache.eviction_policy=lru\n")),
               ConfigError);
}

TEST(CacheOptionsFromConfig, MissingKeyRejected) {
  EXPECT_THROW(Cache::Options::from_config(Config::parse("cache.capacity=1MB\n")), ConfigError);
}

// ---- The real config files in the repo must always be valid ---------------------

TEST(ShippedConfig, NodeConfigIsValid) {
  const Config c = Config::load_file(CACHEGRID_SOURCE_DIR "/config/cachegrid.conf");
  const auto o = Cache::Options::from_config(c);
  EXPECT_NO_THROW(Cache{o});
}

TEST(ShippedConfig, BenchConfigHasAllKeys) {
  const Config c = Config::load_file(CACHEGRID_SOURCE_DIR "/config/bench.conf");
  for (const char* key : {"bench.shard.keys", "bench.shard.value_size", "bench.shard.ops_per_thread",
                          "bench.shard.write_percent"}) {
    EXPECT_NO_THROW(c.get_size(key)) << key;
  }
  EXPECT_NO_THROW(c.get_bytes("bench.shard.capacity"));
  EXPECT_NO_THROW(c.get_list("bench.shard.shard_counts"));
  EXPECT_NO_THROW(c.get_list("bench.shard.thread_counts"));
  EXPECT_NO_THROW(c.get_string("bench.cache_config"));
}

TEST(ShippedConfig, ServerOptionsAreValid) {
  const Config c = Config::load_file(CACHEGRID_SOURCE_DIR "/config/cachegrid.conf");
  EXPECT_NO_THROW(ServerOptions::from_config(c));
}

TEST(ServerOptionsFromConfig, FdReserveMustCoverWorkers) {
  // Rebuild the shipped file with an fd_reserve that's too small for 4 workers.
  std::ifstream f(CACHEGRID_SOURCE_DIR "/config/cachegrid.conf");
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  const auto pos = text.find("server.fd_reserve = ");
  ASSERT_NE(pos, std::string::npos);
  text = text.substr(0, pos) + "server.fd_reserve = 4\n";
  EXPECT_THROW(ServerOptions::from_config(Config::parse(text)), ConfigError);
}
