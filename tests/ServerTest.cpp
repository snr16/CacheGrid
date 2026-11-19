// End-to-end tests: a real server on a random free port, real TCP sockets.

#include "TestNet.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace cachegrid;
using namespace cachegrid::test;

// ---- Basic request/response ------------------------------------------------------

TEST(Server, BindsEphemeralPort) {
  TestServer s;
  EXPECT_NE(s.port(), 0);
}

TEST(Server, PingSetGetDel) {
  TestServer s;
  Client c(s.port());
  EXPECT_EQ(c.command({"PING"}), "+PONG\r\n");
  EXPECT_EQ(c.command({"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(c.command({"GET", "k"}), "$1\r\nv\r\n");
  EXPECT_EQ(c.command({"DEL", "k"}), ":1\r\n");
  EXPECT_EQ(c.command({"GET", "k"}), "$-1\r\n");
}

TEST(Server, InlineCommands) {
  TestServer s;
  Client c(s.port());
  c.send_raw("SET greeting hello\r\nGET greeting\r\n");
  EXPECT_EQ(c.read_reply(), "+OK\r\n");
  EXPECT_EQ(c.read_reply(), "$5\r\nhello\r\n");
}

TEST(Server, PipelinedCommandsAllAnsweredInOrder) {
  TestServer s;
  Client c(s.port());
  const int depth = cfg_int("test.server.pipeline_depth");
  std::string batch;
  for (int i = 0; i < depth; ++i) batch += encode({"SET", "k" + std::to_string(i), std::to_string(i)});
  for (int i = 0; i < depth; ++i) batch += encode({"GET", "k" + std::to_string(i)});
  c.send_raw(batch);
  for (int i = 0; i < depth; ++i) ASSERT_EQ(c.read_reply(), "+OK\r\n");
  for (int i = 0; i < depth; ++i) {
    const std::string v = std::to_string(i);
    ASSERT_EQ(c.read_reply(), "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n");
  }
}

TEST(Server, CommandSentOneByteAtATime) {
  TestServer s;
  Client c(s.port());
  for (char ch : encode({"SET", "slow", "value"})) c.send_raw(std::string_view(&ch, 1));
  EXPECT_EQ(c.read_reply(), "+OK\r\n");
  EXPECT_EQ(c.command({"GET", "slow"}), "$5\r\nvalue\r\n");
}

TEST(Server, LargeAndBinaryValuesRoundTrip) {
  TestServer s;
  Client c(s.port());
  std::string big(512 * 1024, 'x');  // bigger than socket buffers: forces partial writes
  big[100] = '\0';
  big[200] = '\r';
  big[201] = '\n';
  EXPECT_EQ(c.command({"SET", "big", big}), "+OK\r\n");
  EXPECT_EQ(c.command({"GET", "big"}), "$" + std::to_string(big.size()) + "\r\n" + big + "\r\n");
}

// ---- Bad input and misbehaving clients -------------------------------------------

TEST(Server, MalformedRequestGetsErrorAndCloseOthersUnaffected) {
  TestServer s;
  Client bad(s.port());
  Client good(s.port());
  bad.send_raw("*1\r\n+PING\r\n");
  EXPECT_EQ(bad.read_reply().rfind("-ERR Protocol error", 0), 0u);
  EXPECT_TRUE(bad.wait_closed());
  EXPECT_EQ(good.command({"PING"}), "+PONG\r\n");
}

TEST(Server, OversizedRequestRejected) {
  ServerOptions o = test_server_opts();
  o.net.max_request_bytes = 1024;
  TestServer s(o);
  Client c(s.port());
  c.send_raw(encode({"SET", "k", std::string(4096, 'x')}));
  EXPECT_EQ(c.read_reply().rfind("-ERR Protocol error", 0), 0u);
  EXPECT_TRUE(c.wait_closed());
}

TEST(Server, ClientDisconnectMidCommandIsCleanedUp) {
  TestServer s;
  {
    Client c(s.port());
    EXPECT_EQ(c.command({"PING"}), "+PONG\r\n");
    c.send_raw("*3\r\n$3\r\nSET\r\n$1\r\nk");  // half a command, then vanish
  }
  EXPECT_TRUE(eventually([&] { return s.server.active_connections() == 0; }));
  Client c2(s.port());
  EXPECT_EQ(c2.command({"GET", "k"}), "$-1\r\n");
}

TEST(Server, HalfClosedClientStillGetsReplies) {
  TestServer s;
  Client c(s.port());
  c.send_raw("PING\r\n");
  c.shutdown_write();  // "I'm done sending", like `printf 'PING\r\n' | nc`
  EXPECT_EQ(c.read_reply(), "+PONG\r\n");
  EXPECT_TRUE(c.wait_closed());
}

TEST(Server, ClientThatNeverReadsIsDisconnectedAtOutputLimit) {
  ServerOptions o = test_server_opts();
  o.net.max_output_buffer = 64 * 1024;
  TestServer s(o);
  Client c(s.port());
  EXPECT_EQ(c.command({"SET", "v", std::string(32 * 1024, 'x')}), "+OK\r\n");
  std::string flood;
  for (int i = 0; i < 100; ++i) flood += encode({"GET", "v"});  // ~3.2 MB of replies
  c.send_raw(flood);
  EXPECT_TRUE(c.wait_closed());
  EXPECT_TRUE(eventually([&] { return s.server.active_connections() == 0; }));
}

TEST(Server, QuitClosesConnection) {
  TestServer s;
  Client c(s.port());
  EXPECT_EQ(c.command({"QUIT"}), "+OK\r\n");
  EXPECT_TRUE(c.wait_closed());
}

// ---- Limits and concurrency --------------------------------------------------------

TEST(Server, MaxConnectionsEnforced) {
  ServerOptions o = test_server_opts();
  o.net.max_connections = 2;
  TestServer s(o);
  Client a(s.port());
  Client b(s.port());
  ASSERT_EQ(a.command({"PING"}), "+PONG\r\n");
  ASSERT_EQ(b.command({"PING"}), "+PONG\r\n");

  Client extra(s.port());
  EXPECT_EQ(extra.read_reply(), "-ERR max number of clients reached\r\n");
  EXPECT_TRUE(extra.wait_closed());

  a.close();  // frees a slot
  EXPECT_TRUE(eventually([&] { return s.server.active_connections() == 1; }));
  Client again(s.port());
  EXPECT_EQ(again.command({"PING"}), "+PONG\r\n");
}

TEST(Server, ManyConcurrentClients) {
  TestServer s;
  const int kClients = cfg_int("test.server.concurrent_clients");
  const int kOps = cfg_int("test.server.ops_per_client");
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&, t] {
      Client c(s.port());
      for (int i = 0; i < kOps; ++i) {
        const std::string key = "c" + std::to_string(t) + ":" + std::to_string(i);
        if (c.command({"SET", key, key}) != "+OK\r\n") failures.fetch_add(1);
        if (c.command({"GET", key}) != "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n") {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(s.cache.size(), static_cast<std::size_t>(kClients * kOps));
}

TEST(Server, StopClosesOpenConnections) {
  TestServer s;
  Client c(s.port());
  EXPECT_EQ(c.command({"PING"}), "+PONG\r\n");
  s.server.stop();
  EXPECT_TRUE(c.wait_closed());
  EXPECT_EQ(s.server.active_connections(), 0u);
}

TEST(Server, InfoReportsConnections) {
  TestServer s;
  Client c(s.port());
  const std::string info = c.command({"INFO"});
  EXPECT_NE(info.find("connected_clients:1\r\n"), std::string::npos) << info;
  EXPECT_NE(info.find("worker_threads:2\r\n"), std::string::npos) << info;
}

TEST(Server, BackgroundSweeperRemovesExpiredKeys) {
  TestServer s;  // purge interval 50ms, real clock
  Client c(s.port());
  EXPECT_EQ(c.command({"SET", "temp", "v", "PX", "20"}), "+OK\r\n");
  // Nobody reads the key, so only the sweeper can remove it.
  EXPECT_TRUE(eventually([&] { return s.cache.size() == 0; }));
}
