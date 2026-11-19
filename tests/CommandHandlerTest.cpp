#include "cachegrid/CommandHandler.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace cachegrid;
using namespace cachegrid::test;
using namespace std::chrono_literals;

namespace {

class CommandHandlerTest : public ::testing::Test {
 protected:
  CommandHandlerTest() : cache_(make_opts()), handler_(cache_, [](std::string& o) { o += "x:1\r\n"; }) {}

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
