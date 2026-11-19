#include "cachegrid/Cache.h"
#include "cachegrid/CacheServer.h"
#include "cachegrid/Config.h"
#include "cachegrid/LoadGenerator.h"
#include "cachegrid/Workload.h"
#include "cachegrid/ProxyServer.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
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
      "cache.entry_overhead_bytes = 100\n"
      "cache.tombstone_ttl_ms = 5000\n");
  const auto o = Cache::Options::from_config(c);
  EXPECT_EQ(o.capacity_bytes, 1u << 20);
  EXPECT_EQ(o.num_shards, 8u);
  EXPECT_EQ(o.policy, EvictionPolicy::NoEviction);
  EXPECT_EQ(o.entry_overhead_bytes, 100u);
  EXPECT_EQ(o.tombstone_ttl, Millis(5000));
}

TEST(CacheOptionsFromConfig, RejectsBadPolicyAndZeroShards) {
  const std::string base = "cache.capacity=1MB\ncache.entry_overhead_bytes=96\ncache.tombstone_ttl_ms=1000\n";
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

// ---- include / override ---------------------------------------------------------------

namespace {
// Writes files into a fresh temp directory for include tests.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/cachegrid-config-test-XXXXXX";
    path_ = ::mkdtemp(tmpl);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  std::string write(const std::string& name, const std::string& text) const {
    const std::string p = path_ + "/" + name;
    std::ofstream(p) << text;
    return p;
  }

 private:
  std::string path_;
};
}  // namespace

TEST(ConfigInclude, IncludedValuesCanBeOverridden) {
  TempDir d;
  d.write("base.conf", "a = 1\nb = 2\n");
  const Config c = Config::load_file(d.write("child.conf", "include base.conf\nb = 20\nc = 3\n"));
  EXPECT_EQ(c.get_int("a"), 1);
  EXPECT_EQ(c.get_int("b"), 20);
  EXPECT_EQ(c.get_int("c"), 3);
}

TEST(ConfigInclude, DuplicateInSameFileStillRejected) {
  TempDir d;
  d.write("base.conf", "a = 1\n");
  EXPECT_THROW(Config::load_file(d.write("child.conf", "include base.conf\nb = 1\nb = 2\n")), ConfigError);
}

TEST(ConfigInclude, IncludeAfterSettingsRejected) {
  TempDir d;
  d.write("base.conf", "a = 1\n");
  EXPECT_THROW(Config::load_file(d.write("child.conf", "a = 5\ninclude base.conf\n")), ConfigError);
}

TEST(ConfigInclude, CycleRejected) {
  TempDir d;
  d.write("a.conf", "include b.conf\n");
  d.write("b.conf", "include a.conf\n");
  EXPECT_THROW(Config::load_file(d.write("start.conf", "include a.conf\n")), ConfigError);
}

TEST(ConfigInclude, MissingIncludeRejected) {
  TempDir d;
  EXPECT_THROW(Config::load_file(d.write("child.conf", "include nope.conf\n")), ConfigError);
}

// ---- Every shipped config must load ---------------------------------------------------

TEST(ShippedConfig, LayeredNodeConfigsAreValid) {
  for (const char* f : {"/config/local/node2.conf", "/config/local/node3.conf", "/config/docker/node.conf"}) {
    const Config c = Config::load_file(std::string(CACHEGRID_SOURCE_DIR) + f);
    EXPECT_NO_THROW(Cache::Options::from_config(c)) << f;
    EXPECT_NO_THROW(ServerOptions::from_config(c)) << f;
  }
  EXPECT_EQ(Config::load_file(CACHEGRID_SOURCE_DIR "/config/local/node3.conf").get_size("server.port"), 7003u);
  EXPECT_EQ(Config::load_file(CACHEGRID_SOURCE_DIR "/config/docker/node.conf").get_string("server.bind_address"),
            "0.0.0.0");
}

TEST(ShippedConfig, ProxyConfigsAreValid) {
  for (const char* f : {"/config/proxy.conf", "/config/docker/proxy.conf"}) {
    const ProxyOptions o = ProxyOptions::from_config(Config::load_file(std::string(CACHEGRID_SOURCE_DIR) + f));
    EXPECT_EQ(o.nodes.size(), 3u) << f;
  }
}

TEST(ShippedConfig, RingBenchKeysPresent) {
  const Config c = Config::load_file(CACHEGRID_SOURCE_DIR "/config/bench.conf");
  EXPECT_NO_THROW(c.get_size("bench.ring.keys"));
  EXPECT_NO_THROW(c.get_size("bench.ring.nodes"));
  EXPECT_NO_THROW(c.get_size("bench.ring.change_vnodes"));
  EXPECT_NO_THROW(c.get_list("bench.ring.vnode_counts"));
}

// ---- Proxy node list parsing -----------------------------------------------------------

TEST(ProxyNodes, ParsesIdHostPort) {
  const auto n = ProxyOptions::parse_nodes({"a@10.0.0.1:7001", "b@node-b:7002"});
  ASSERT_EQ(n.size(), 2u);
  EXPECT_EQ(n[1].id, "b");
  EXPECT_EQ(n[1].host, "node-b");
  EXPECT_EQ(n[1].port, 7002);
}

TEST(ProxyNodes, RejectsMalformedAndDuplicates) {
  for (std::string bad : {"nohost", "a@host", "a@host:0", "a@host:99999", "@host:1", "a@:1", "a@host:12x"}) {
    EXPECT_THROW(ProxyOptions::parse_nodes({bad}), ConfigError) << bad;
  }
  EXPECT_THROW(ProxyOptions::parse_nodes({"a@h:1", "a@h:2"}), ConfigError);
  EXPECT_THROW(ProxyOptions::parse_nodes({}), ConfigError);
}

TEST(ShippedConfig, EveryBenchmarkScenarioIsValid) {
  const std::string dir = std::string(CACHEGRID_SOURCE_DIR) + "/config/bench";
  std::size_t scenarios = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".conf") continue;
    ++scenarios;
    const Config c = Config::load_file(entry.path().string());
    const std::string name = entry.path().filename().string();
    EXPECT_NO_THROW(load_options_from_config(c)) << name;
    EXPECT_NO_THROW(c.get_string("bench.scenario.name")) << name;
    EXPECT_NO_THROW(c.get_list("bench.load.concurrency_levels")) << name;
    EXPECT_NO_THROW(c.get_bool("bench.workload.preload")) << name;
    const std::string type = c.get_string("bench.workload.type");
    EXPECT_TRUE(type == "uniform" || type == "zipf" || type == "trace") << name;
    if (type != "trace") EXPECT_NO_THROW(make_workload_from_config(c)) << name;  // trace file is optional data
  }
  EXPECT_GE(scenarios, 7u);
}
