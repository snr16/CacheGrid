#include "cachegrid/CommandHandler.h"
#include "cachegrid/Resp.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace cachegrid;
using namespace cachegrid::test;
using namespace std::chrono_literals;

namespace {

class CommandHandlerTest : public ::testing::Test {
 protected:
  CommandHandlerTest() : cache_(make_opts()), handler_(cache_, [](std::string& o) { o += "x:1\r\n"; }, "test-run-id") {}

  Cache::Options make_opts() {
    Cache::Options o = cache_opts(cfg_bytes("test.handler.capacity"), cfg_size("test.handler.shards"));
    o.now = clock_.fn();
    return o;
  }

  // Runs one command and returns the raw RESP reply.
  std::string run(std::vector<std::string> args) {
    std::string out;
    last_action_ = handler_.execute(args, out);
    return out;
  }

  FakeClock clock_;
  Cache cache_;
  CommandHandler handler_;
  CommandHandler::Action last_action_ = CommandHandler::Action::Continue;
};

}  // namespace

TEST_F(CommandHandlerTest, Ping) {
  EXPECT_EQ(run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(run({"ping", "hey"}), "$3\r\nhey\r\n");
}

TEST_F(CommandHandlerTest, SetGetRoundTrip) {
  EXPECT_EQ(run({"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
}

TEST_F(CommandHandlerTest, GetMissingReturnsNull) {
  EXPECT_EQ(run({"GET", "nope"}), "$-1\r\n");
}

TEST_F(CommandHandlerTest, CommandNamesAreCaseInsensitive) {
  EXPECT_EQ(run({"sEt", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(run({"get", "k"}), "$1\r\nv\r\n");
}

TEST_F(CommandHandlerTest, SetWithPxExpires) {
  run({"SET", "k", "v", "PX", "100"});
  clock_.advance(99ms);
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
  clock_.advance(1ms);
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandHandlerTest, SetWithExUsesSeconds) {
  run({"SET", "k", "v", "ex", "2"});
  clock_.advance(1999ms);
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
  clock_.advance(1ms);
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandHandlerTest, SetRejectsBadExpiry) {
  const std::string err = "-ERR invalid expire time in 'set' command\r\n";
  EXPECT_EQ(run({"SET", "k", "v", "PX", "0"}), err);
  EXPECT_EQ(run({"SET", "k", "v", "PX", "-5"}), err);
  EXPECT_EQ(run({"SET", "k", "v", "PX", "abc"}), err);
  EXPECT_EQ(run({"SET", "k", "v", "EX", "999999999999999999"}), err);  // would overflow
  EXPECT_EQ(run({"SET", "k", "v", "PX", "999999999999999999"}), err);  // past clock range
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n") << "failed SET must not store anything";
}

TEST_F(CommandHandlerTest, SetRejectsUnknownOption) {
  EXPECT_EQ(run({"SET", "k", "v", "XX", "1"}), "-ERR syntax error\r\n");
}

TEST_F(CommandHandlerTest, SetTooLargeForShard) {
  // One byte more than a whole shard can hold.
  const std::size_t shard = cfg_bytes("test.handler.capacity") / cfg_size("test.handler.shards");
  EXPECT_EQ(run({"SET", "k", std::string(shard + 1, 'x')}),
            "-ERR value too large for cache shard\r\n");
}

TEST_F(CommandHandlerTest, DelCountsRemovedKeys) {
  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"DEL", "a", "b", "c"}), ":2\r\n");
  EXPECT_EQ(run({"DEL", "a"}), ":0\r\n");
}

TEST_F(CommandHandlerTest, DbSize) {
  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"DBSIZE"}), ":2\r\n");
}

TEST_F(CommandHandlerTest, WrongArity) {
  EXPECT_EQ(run({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
  EXPECT_EQ(run({"GET", "a", "b"}), "-ERR wrong number of arguments for 'get' command\r\n");
  EXPECT_EQ(run({"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
  EXPECT_EQ(run({"DEL"}), "-ERR wrong number of arguments for 'del' command\r\n");
  EXPECT_EQ(run({"PING", "a", "b"}), "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST_F(CommandHandlerTest, UnknownCommand) {
  EXPECT_EQ(run({"FLY", "away"}), "-ERR unknown command 'FLY'\r\n");
  EXPECT_EQ(last_action_, CommandHandler::Action::Continue);
}

TEST_F(CommandHandlerTest, QuitAsksToClose) {
  EXPECT_EQ(run({"QUIT"}), "+OK\r\n");
  EXPECT_EQ(last_action_, CommandHandler::Action::Close);
}

TEST_F(CommandHandlerTest, InfoIncludesServerAndCacheStats) {
  run({"SET", "a", "1"});
  run({"GET", "a"});
  run({"GET", "zz"});
  const std::string reply = run({"INFO"});
  EXPECT_EQ(reply[0], '$');
  EXPECT_NE(reply.find("x:1\r\n"), std::string::npos) << "server section from callback";
  EXPECT_NE(reply.find("keyspace_hits:1\r\n"), std::string::npos);
  EXPECT_NE(reply.find("keyspace_misses:1\r\n"), std::string::npos);
  EXPECT_NE(reply.find("keys:1\r\n"), std::string::npos);
}

TEST_F(CommandHandlerTest, CommandReturnsEmptyArrayForRedisCli) {
  EXPECT_EQ(run({"COMMAND", "DOCS"}), "*0\r\n");
}

// ---- Internal replication commands -------------------------------------------

TEST_F(CommandHandlerTest, VsetAppliesOnlyNewerVersions) {
  EXPECT_EQ(run({"VSET", "10", "k", "a"}), "+OK\r\n");
  EXPECT_EQ(run({"VSET", "9", "k", "old"}), "+STALE\r\n");
  EXPECT_EQ(run({"VSET", "11", "k", "b", "PX", "100"}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nb\r\n");
  clock_.advance(Millis(100));
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandHandlerTest, VsetRejectsBadVersionAndArity) {
  EXPECT_EQ(run({"VSET", "0", "k", "v"}), "-ERR invalid version\r\n");
  EXPECT_EQ(run({"VSET", "abc", "k", "v"}), "-ERR invalid version\r\n");
  EXPECT_EQ(run({"VSET", "99999999999999999999999", "k", "v"}), "-ERR invalid version\r\n");
  EXPECT_EQ(run({"VSET", "1", "k"}), "-ERR wrong number of arguments for 'vset' command\r\n");
}

TEST_F(CommandHandlerTest, VdelLeavesTombstone) {
  run({"VSET", "10", "k", "v"});
  EXPECT_EQ(run({"VDEL", "20", "k"}), ":1\r\n");
  EXPECT_EQ(run({"VDEL", "21", "k"}), ":0\r\n") << "already deleted";
  EXPECT_EQ(run({"VSET", "15", "k", "zombie"}), "+STALE\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandHandlerTest, ScanReturnsShardEntries) {
  run({"VSET", "42", "k", "v"});
  const std::size_t shard = cache_.shard_index("k");
  const std::string reply = run({"SCAN", std::to_string(shard)});
  resp::Value v;
  ASSERT_TRUE(resp::decode_reply(reply, v)) << reply;
  ASSERT_EQ(v.elements.size(), 2u);
  EXPECT_EQ(static_cast<std::size_t>(v.elements[0].integer), cache_.num_shards());
  ASSERT_EQ(v.elements[1].elements.size(), 1u);
  const auto& e = v.elements[1].elements[0];
  EXPECT_EQ(e.elements[0].str, "k");
  EXPECT_EQ(e.elements[1].str, "42");
  EXPECT_EQ(e.elements[2].integer, -1) << "no TTL";
  EXPECT_EQ(e.elements[3].integer, 0) << "not a tombstone";
  EXPECT_EQ(e.elements[4].str, "v");
  EXPECT_EQ(run({"SCAN", std::to_string(cache_.num_shards())}), "-ERR shard index out of range\r\n");
}

TEST_F(CommandHandlerTest, FlushallAndRunid) {
  run({"SET", "a", "1"});
  EXPECT_EQ(run({"FLUSHALL"}), "+OK\r\n");
  EXPECT_EQ(run({"DBSIZE"}), ":0\r\n");
  EXPECT_EQ(run({"RUNID"}), "$11\r\ntest-run-id\r\n");
}
