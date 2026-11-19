// End-to-end tests: real cache nodes + a real proxy, all in-process on random
// ports, talking over TCP.

#include "cachegrid/ProxyServer.h"
#include "TestNet.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

using namespace cachegrid;
using namespace cachegrid::test;

namespace {

// Proxy settings for tests, from `test.proxy.*` in config/test.conf.
ProxyOptions test_proxy_opts(std::vector<NodeAddress> nodes) {
  ProxyOptions o{};
  o.net = test_listener_opts();
  o.nodes = std::move(nodes);
  o.vnodes_per_node = cfg_size("test.proxy.vnodes_per_node");
  o.connect_timeout = cfg_ms("test.proxy.connect_timeout_ms");
  o.request_timeout = cfg_ms("test.proxy.request_timeout_ms");
  o.reconnect_backoff = cfg_ms("test.proxy.reconnect_backoff_ms");
  o.timer_tick = cfg_ms("test.proxy.timer_tick_ms");
  o.max_pending_per_client = cfg_size("test.proxy.max_pending_per_client");
  o.max_reply_bytes = cfg_bytes("test.proxy.max_reply_bytes");
  return o;
}

std::string bulk(const std::string& v) { return "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n"; }

// N cache nodes ("node1".."nodeN") behind one proxy.
class Cluster {
 public:
  explicit Cluster(const std::function<void(ProxyOptions&)>& tweak = {}) {
    const std::size_t n = cfg_size("test.proxy.nodes");
    std::vector<NodeAddress> addrs;
    for (std::size_t i = 0; i < n; ++i) {
      nodes_.push_back(std::make_unique<TestServer>());
      addrs.push_back(NodeAddress{id(i), "127.0.0.1", nodes_.back()->port()});
    }
    ProxyOptions o = test_proxy_opts(addrs);
    if (tweak) tweak(o);
    proxy_ = std::make_unique<ProxyServer>(o);
    proxy_->start();
  }

  static std::string id(std::size_t i) { return "node" + std::to_string(i + 1); }
  std::size_t index_of(const std::string& node_id) const { return std::stoul(node_id.substr(4)) - 1; }

  ProxyServer& proxy() { return *proxy_; }
  std::uint16_t port() const { return proxy_->port(); }
  TestServer& node(std::size_t i) { return *nodes_[i]; }
  Cache& cache_of(const std::string& node_id) { return nodes_[index_of(node_id)]->cache; }

  // Brings a node back on the same port with an empty cache.
  void restart(std::size_t i) {
    const std::uint16_t port = nodes_[i]->port();
    nodes_[i].reset();
    ServerOptions o = test_server_opts();
    o.net.port = port;
    nodes_[i] = std::make_unique<TestServer>(o);
  }

  // A key whose owner is `node_id`.
  std::string key_on(const std::string& node_id) {
    for (int i = 0;; ++i) {
      std::string k = "probe:" + std::to_string(i);
      if (proxy_->owner_of(k) == node_id) return k;
    }
  }

 private:
  std::vector<std::unique_ptr<TestServer>> nodes_;
  std::unique_ptr<ProxyServer> proxy_;
};

}  // namespace

// ---- Routing --------------------------------------------------------------------------

TEST(Proxy, SetGetThroughProxy) {
  Cluster c;
  Client cl(c.port());
  EXPECT_EQ(cl.command({"SET", "user:1", "alice"}), "+OK\r\n");
  EXPECT_EQ(cl.command({"GET", "user:1"}), bulk("alice"));
  EXPECT_EQ(cl.command({"GET", "missing"}), "$-1\r\n");
}

TEST(Proxy, EachKeyStoredOnlyOnItsRingOwner) {
  Cluster c;
  Client cl(c.port());
  for (int i = 0; i < 300; ++i) {
    const std::string k = "k" + std::to_string(i);
    ASSERT_EQ(cl.command({"SET", k, k}), "+OK\r\n");
  }
  for (int i = 0; i < 300; ++i) {
    const std::string k = "k" + std::to_string(i);
    const std::string owner = c.proxy().owner_of(k);
    for (std::size_t n = 0; n < 3; ++n) {
      const bool has = c.node(n).cache.get(k).has_value();
      ASSERT_EQ(has, Cluster::id(n) == owner) << k << " on " << Cluster::id(n);
    }
  }
}

TEST(Proxy, KeysSpreadAcrossAllNodes) {
  Cluster c;
  Client cl(c.port());
  for (int i = 0; i < 600; ++i) cl.send_raw(encode({"SET", "k" + std::to_string(i), "v"}));
  for (int i = 0; i < 600; ++i) ASSERT_EQ(cl.read_reply(), "+OK\r\n");
  std::size_t total = 0;
  for (std::size_t n = 0; n < 3; ++n) {
    EXPECT_GT(c.node(n).cache.size(), 100u) << Cluster::id(n) << " got too few keys";
    total += c.node(n).cache.size();
  }
  EXPECT_EQ(total, 600u);
}

TEST(Proxy, PipelinedRepliesStayInOrderAcrossNodes) {
  // Replies from three nodes arrive interleaved; the client must still get
  // them in exactly the order it sent the commands.
  Cluster c;
  Client cl(c.port());
  std::string batch;
  std::vector<std::string> expected;
  for (int i = 0; i < cfg_int("test.proxy.pipeline_depth"); ++i) {
    const std::string k = "order:" + std::to_string(i);
    batch += encode({"SET", k, std::to_string(i)});
    expected.push_back("+OK\r\n");
    batch += encode({"GET", k});
    expected.push_back(bulk(std::to_string(i)));
    if (i % 10 == 0) {
      batch += encode({"PING"});  // answered by the proxy itself, mid-stream
      expected.push_back("+PONG\r\n");
    }
  }
  cl.send_raw(batch);
  for (std::size_t i = 0; i < expected.size(); ++i) ASSERT_EQ(cl.read_reply(), expected[i]) << "reply " << i;
}

TEST(Proxy, MultiKeyDelSpansNodes) {
  Cluster c;
  Client cl(c.port());
  std::vector<std::string> del = {"DEL"};
  for (int i = 0; i < 50; ++i) {
    const std::string k = "d" + std::to_string(i);
    cl.command({"SET", k, "v"});
    del.push_back(k);
  }
  for (int i = 0; i < 10; ++i) del.push_back("never-set-" + std::to_string(i));
  EXPECT_EQ(cl.command(del), ":50\r\n");
  EXPECT_EQ(cl.command({"DBSIZE"}), ":0\r\n");
}

TEST(Proxy, DbsizeSumsAllNodes) {
  Cluster c;
  Client cl(c.port());
  for (int i = 0; i < 90; ++i) cl.command({"SET", "s" + std::to_string(i), "v"});
  EXPECT_EQ(cl.command({"DBSIZE"}), ":90\r\n");
}

TEST(Proxy, LocalCommands) {
  Cluster c;
  Client cl(c.port());
  EXPECT_EQ(cl.command({"PING"}), "+PONG\r\n");
  EXPECT_EQ(cl.command({"PING", "hi"}), bulk("hi"));
  EXPECT_EQ(cl.command({"DEL"}), "-ERR wrong number of arguments for 'del' command\r\n");
  EXPECT_EQ(cl.command({"NOPE"}), "-ERR unknown command 'NOPE'\r\n");
  const std::string info = cl.command({"INFO"});
  EXPECT_NE(info.find("nodes:3\r\n"), std::string::npos) << info;
  EXPECT_EQ(cl.command({"QUIT"}), "+OK\r\n");
  EXPECT_TRUE(cl.wait_closed());
}

TEST(Proxy, NodeErrorsArePassedThrough) {
  Cluster c;
  Client cl(c.port());
  // Arity is checked by the node; the proxy relays its answer unchanged.
  EXPECT_EQ(cl.command({"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
  EXPECT_EQ(cl.command({"SET", "k", "v", "PX", "0"}), "-ERR invalid expire time in 'set' command\r\n");
}

// ---- Failures -----------------------------------------------------------------------------

TEST(Proxy, DeadNodeGivesErrorWhileOthersKeepWorking) {
  Cluster c;
  Client cl(c.port());
  const std::string on2 = c.key_on("node2");
  const std::string on1 = c.key_on("node1");
  ASSERT_EQ(cl.command({"SET", on1, "alive"}), "+OK\r\n");

  c.node(1).server.stop();
  const std::string reply = cl.command({"GET", on2});
  EXPECT_EQ(reply.rfind("-ERR node node2 ", 0), 0u) << reply;
  EXPECT_EQ(cl.command({"GET", on1}), bulk("alive"));
}

TEST(Proxy, RecoversWhenNodeComesBack) {
  Cluster c;
  Client cl(c.port());
  const std::string k = c.key_on("node2");
  c.node(1).server.stop();
  ASSERT_EQ(cl.command({"GET", k}).rfind("-ERR node node2 ", 0), 0u);

  c.restart(1);
  // After the reconnect backoff the proxy tries again and succeeds.
  EXPECT_TRUE(eventually([&] { return cl.command({"SET", k, "back"}) == "+OK\r\n"; }));
  EXPECT_EQ(cl.command({"GET", k}), bulk("back"));
}

TEST(Proxy, HungNodeTimesOut) {
  // A "node" that accepts TCP connections (kernel backlog) but never answers.
  std::uint16_t hung_port = 0;
  const ListenerOptions l = test_listener_opts();
  const int hung = net::create_listener(l.bind_address, l.port, l.listen_backlog, hung_port);
  ProxyOptions o = test_proxy_opts({NodeAddress{"stuck", "127.0.0.1", hung_port}});
  ProxyServer proxy(o);
  proxy.start();

  Client cl(proxy.port());
  const auto start = std::chrono::steady_clock::now();
  const std::string reply = cl.command({"GET", "k"});
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(reply.rfind("-ERR node stuck ", 0), 0u) << reply;
  EXPECT_GE(waited, o.request_timeout);
  EXPECT_LT(waited, o.request_timeout * 4);
  proxy.stop();
  ::close(hung);
}

TEST(Proxy, ClientLeavingWithRequestsInFlightIsSafe) {
  std::uint16_t hung_port = 0;
  const ListenerOptions l = test_listener_opts();
  const int hung = net::create_listener(l.bind_address, l.port, l.listen_backlog, hung_port);
  ProxyServer proxy(test_proxy_opts({NodeAddress{"stuck", "127.0.0.1", hung_port}}));
  proxy.start();
  {
    Client cl(proxy.port());
    for (int i = 0; i < 20; ++i) cl.send_raw(encode({"GET", "k" + std::to_string(i)}));
    ASSERT_TRUE(eventually([&] { return proxy.active_connections() == 1; }));
  }  // client disconnects while all 20 requests are still waiting
  EXPECT_TRUE(eventually([&] { return proxy.active_connections() == 0; }));
  Client other(proxy.port());
  EXPECT_EQ(other.command({"PING"}), "+PONG\r\n");
  proxy.stop();
  ::close(hung);
}

// ---- Membership ---------------------------------------------------------------------------

TEST(Proxy, MembershipChangeReroutesNewRequests) {
  Cluster c;
  Client cl(c.port());
  c.proxy().set_nodes({NodeAddress{"node1", "127.0.0.1", c.node(0).port()},
                       NodeAddress{"node2", "127.0.0.1", c.node(1).port()}});
  for (int i = 0; i < 200; ++i) {
    const std::string k = "m" + std::to_string(i);
    ASSERT_NE(c.proxy().owner_of(k), "node3");
    ASSERT_EQ(cl.command({"SET", k, "v"}), "+OK\r\n");
  }
  EXPECT_EQ(c.node(2).cache.size(), 0u) << "removed node must receive nothing";
  EXPECT_EQ(c.node(0).cache.size() + c.node(1).cache.size(), 200u);
}

TEST(Proxy, EmptyMembershipRejected) {
  Cluster c;
  EXPECT_THROW(c.proxy().set_nodes({}), std::invalid_argument);
}

// ---- Load and back-pressure ----------------------------------------------------------------

TEST(Proxy, ManyConcurrentClients) {
  Cluster c;
  const int kClients = cfg_int("test.proxy.concurrent_clients");
  const int kOps = cfg_int("test.proxy.ops_per_client");
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&, t] {
      Client cl(c.port());
      for (int i = 0; i < kOps; ++i) {
        const std::string k = "c" + std::to_string(t) + ":" + std::to_string(i);
        if (cl.command({"SET", k, k}) != "+OK\r\n") failures.fetch_add(1);
        if (cl.command({"GET", k}) != bulk(k)) failures.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
  std::size_t total = 0;
  for (std::size_t n = 0; n < 3; ++n) total += c.node(n).cache.size();
  EXPECT_EQ(total, static_cast<std::size_t>(kClients * kOps));
}

TEST(Proxy, SmallInFlightLimitStillAnswersEverythingInOrder) {
  Cluster c([](ProxyOptions& o) { o.max_pending_per_client = cfg_size("test.proxy.small_pending_limit"); });
  Client cl(c.port());
  const int depth = cfg_int("test.server.pipeline_depth");
  std::string batch;
  for (int i = 0; i < depth; ++i) batch += encode({"SET", "bp" + std::to_string(i), std::to_string(i)});
  for (int i = 0; i < depth; ++i) batch += encode({"PING"});
  for (int i = 0; i < depth; ++i) batch += encode({"GET", "bp" + std::to_string(i)});
  cl.send_raw(batch);
  for (int i = 0; i < depth; ++i) ASSERT_EQ(cl.read_reply(), "+OK\r\n");
  for (int i = 0; i < depth; ++i) ASSERT_EQ(cl.read_reply(), "+PONG\r\n");
  for (int i = 0; i < depth; ++i) ASSERT_EQ(cl.read_reply(), bulk(std::to_string(i)));
}

TEST(Proxy, MalformedRequestClosesOnlyThatClient) {
  Cluster c;
  Client bad(c.port());
  Client good(c.port());
  bad.send_raw("*1\r\n+PING\r\n");
  EXPECT_EQ(bad.read_reply().rfind("-ERR Protocol error", 0), 0u);
  EXPECT_TRUE(bad.wait_closed());
  EXPECT_EQ(good.command({"PING"}), "+PONG\r\n");
}
