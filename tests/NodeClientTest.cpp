#include "cachegrid/NodeClient.h"
#include "TestNet.h"

#include <gtest/gtest.h>

using namespace cachegrid;
using namespace cachegrid::test;

TEST(NodeClient, PipelinedCallsGetOneReplyEach) {
  TestServer s;
  NodeClient c("127.0.0.1", s.port(), cfg_ms("test.proxy.health_timeout_ms"), cfg_bytes("test.proxy.max_reply_bytes"),
               cfg_bytes("test.listener.read_chunk_bytes"));
  std::vector<std::string> replies;
  ASSERT_TRUE(c.call({{"SET", "a", "1"}, {"GET", "a"}, {"PING"}}, replies));
  EXPECT_EQ(replies, (std::vector<std::string>{"+OK\r\n", "$1\r\n1\r\n", "+PONG\r\n"}));
}

TEST(NodeClient, FailsFastWhenNothingListens) {
  std::uint16_t port = 0;
  {
    TestServer s;
    port = s.port();
  }  // server gone: connection refused
  NodeClient c("127.0.0.1", port, cfg_ms("test.proxy.health_timeout_ms"), cfg_bytes("test.proxy.max_reply_bytes"),
               cfg_bytes("test.listener.read_chunk_bytes"));
  std::string reply;
  EXPECT_FALSE(c.call({"PING"}, reply));
}

TEST(NodeClient, TimesOutOnSilentServer) {
  std::uint16_t port = 0;
  const ListenerOptions l = test_listener_opts();
  const int hung = net::create_listener(l.bind_address, l.port, l.listen_backlog, port);
  const Millis timeout = cfg_ms("test.proxy.health_timeout_ms");
  NodeClient c("127.0.0.1", port, timeout, cfg_bytes("test.proxy.max_reply_bytes"),
               cfg_bytes("test.listener.read_chunk_bytes"));
  const auto start = std::chrono::steady_clock::now();
  std::string reply;
  EXPECT_FALSE(c.call({"PING"}, reply));
  EXPECT_GE(std::chrono::steady_clock::now() - start, timeout);
  ::close(hung);
}

TEST(NodeClient, ReconnectsAfterFailure) {
  TestServer s;
  NodeClient c("127.0.0.1", s.port(), cfg_ms("test.proxy.health_timeout_ms"), cfg_bytes("test.proxy.max_reply_bytes"),
               cfg_bytes("test.listener.read_chunk_bytes"));
  std::string reply;
  ASSERT_TRUE(c.call({"PING"}, reply));
  s.stop();
  EXPECT_FALSE(c.call({"PING"}, reply));
  s.resume();
  EXPECT_TRUE(c.call({"PING"}, reply));
  EXPECT_EQ(reply, "+PONG\r\n");
}
