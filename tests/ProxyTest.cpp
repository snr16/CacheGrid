// End-to-end tests: real cache nodes + a real proxy, all in-process on random
// ports, talking over TCP.

#include "cachegrid/ProxyServer.h"
#include "TestNet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
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
  o.proxy_id = static_cast<std::uint8_t>(cfg_size("test.proxy.id"));
  o.replication_factor = cfg_size("test.proxy.replication_factor");
  o.write_quorum = cfg_size("test.proxy.write_quorum");
  o.health_interval = cfg_ms("test.proxy.health_interval_ms");
  o.health_timeout = cfg_ms("test.proxy.health_timeout_ms");
  o.health_fail_threshold = cfg_size("test.proxy.health_fail_threshold");
  o.recovery_grace = cfg_ms("test.proxy.recovery_grace_ms");
  o.resync_batch = cfg_size("test.proxy.resync_batch");
  o.log_events = cfg().get_bool("test.proxy.log_events");
  return o;
}

const std::size_t kRf = cfg_size("test.proxy.replication_factor");

// Cluster tweak: one copy per key (the Phase 4 behaviour) for tests about a
// single node failing.
void single_copy(ProxyOptions& o) {
  o.replication_factor = 1;
  o.write_quorum = 1;
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

  NodeAddress address(std::size_t i) const { return NodeAddress{id(i), "127.0.0.1", nodes_[i]->port()}; }
  std::size_t size() const { return nodes_.size(); }

  // Starts one more node (not yet a member); returns its address.
  NodeAddress add_node() {
    nodes_.push_back(std::make_unique<TestServer>());
    return address(nodes_.size() - 1);
  }

  bool wait_state(const std::string& node_id, const std::string& state) {
    return eventually([&] { return proxy_->node_state(node_id) == state; });
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

TEST(Proxy, EachKeyStoredOnExactlyItsReplicas) {
  Cluster c;
  Client cl(c.port());
  const int keys = cfg_int("test.proxy.replication_keys");
  for (int i = 0; i < keys; ++i) {
    const std::string k = "k" + std::to_string(i);
    ASSERT_EQ(cl.command({"SET", k, k}), "+OK\r\n");
  }
  for (int i = 0; i < keys; ++i) {
    const std::string k = "k" + std::to_string(i);
    const auto owners = c.proxy().owners_of(k);
    ASSERT_EQ(owners.size(), kRf);
    for (std::size_t n = 0; n < 3; ++n) {
      const bool has = c.node(n).cache.get(k).has_value();
      const bool should = std::find(owners.begin(), owners.end(), Cluster::id(n)) != owners.end();
      ASSERT_EQ(has, should) << k << " on " << Cluster::id(n);
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
  EXPECT_EQ(total, 600u * kRf) << "every key is stored replication_factor times";
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
  // DBSIZE through the proxy counts every stored copy.
  EXPECT_EQ(cl.command({"DBSIZE"}), ":" + std::to_string(90 * kRf) + "\r\n");
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
  // Errors match a node's, whether produced by the proxy (arity) or relayed
  // from the nodes (invalid expiry).
  EXPECT_EQ(cl.command({"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
  EXPECT_EQ(cl.command({"SET", "k", "v", "PX", "0"}), "-ERR invalid expire time in 'set' command\r\n");
}

// ---- Failures -----------------------------------------------------------------------------

TEST(Proxy, SingleCopyDeadNodeGivesErrorWhileOthersKeepWorking) {
  Cluster c(single_copy);
  Client cl(c.port());
  const std::string on2 = c.key_on("node2");
  const std::string on1 = c.key_on("node1");
  ASSERT_EQ(cl.command({"SET", on1, "alive"}), "+OK\r\n");

  c.node(1).server->stop();
  const std::string reply = cl.command({"GET", on2});
  EXPECT_EQ(reply.rfind("-ERR node node2 ", 0), 0u) << reply;
  EXPECT_EQ(cl.command({"GET", on1}), bulk("alive"));
}

TEST(Proxy, SingleCopyReconnectsWhenNodeComesBack) {
  Cluster c(single_copy);
  Client cl(c.port());
  const std::string k = c.key_on("node2");
  c.node(1).server->stop();
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
  EXPECT_EQ(c.node(0).cache.size() + c.node(1).cache.size(), 200u * kRf);
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
  EXPECT_EQ(total, static_cast<std::size_t>(kClients * kOps) * kRf);
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

// ---- Replication & recovery (Phase 5) -------------------------------------------------------

namespace {

std::optional<ExportedEntry> find_entry(const Cache& cache, const std::string& key) {
  for (std::size_t sh = 0; sh < cache.num_shards(); ++sh) {
    for (auto& e : cache.export_shard(sh)) {
      if (e.key == key) return e;
    }
  }
  return std::nullopt;
}

bool holds_copy(ProxyServer& p, const std::string& key, const std::string& node_id) {
  const auto o = p.owners_of(key);
  return std::find(o.begin(), o.end(), node_id) != o.end();
}

// Keys "<prefix><i>" (i < test.proxy.replication_keys) that `node_id` holds a copy of.
std::vector<std::string> keys_held_by(ProxyServer& p, const std::string& node_id, const std::string& prefix) {
  std::vector<std::string> out;
  for (int i = 0; i < cfg_int("test.proxy.replication_keys"); ++i) {
    const std::string k = prefix + std::to_string(i);
    if (holds_copy(p, k, node_id)) out.push_back(k);
  }
  return out;
}

void write_keys(Client& cl, const std::string& prefix, const std::string& value) {
  const int n = cfg_int("test.proxy.replication_keys");
  std::string batch;
  for (int i = 0; i < n; ++i) batch += encode({"SET", prefix + std::to_string(i), value});
  cl.send_raw(batch);
  for (int i = 0; i < n; ++i) ASSERT_EQ(cl.read_reply(), "+OK\r\n");
}

}  // namespace

TEST(Replication, CopiesShareOneVersionAndNewerWritesGetHigherOnes) {
  Cluster c;
  Client cl(c.port());
  ASSERT_EQ(cl.command({"SET", "k", "a"}), "+OK\r\n");
  const auto owners = c.proxy().owners_of("k");
  ASSERT_EQ(owners.size(), kRf);
  const auto first = find_entry(c.cache_of(owners[0]), "k");
  ASSERT_TRUE(first.has_value());
  EXPECT_GT(first->version, 0u);
  for (const auto& o : owners) {
    const auto e = find_entry(c.cache_of(o), "k");
    ASSERT_TRUE(e.has_value()) << o;
    EXPECT_EQ(e->version, first->version) << "every copy carries the same version";
    EXPECT_EQ(e->value, "a");
  }
  ASSERT_EQ(cl.command({"SET", "k", "b"}), "+OK\r\n");
  EXPECT_GT(find_entry(c.cache_of(owners[0]), "k")->version, first->version);
}

TEST(Replication, ReadFailsOverToReplicaImmediately) {
  Cluster c;
  Client cl(c.port());
  const std::string k = c.key_on("node2");  // node2 is the first choice for reads
  ASSERT_EQ(cl.command({"SET", k, "safe"}), "+OK\r\n");
  c.node(1).stop();
  // No waiting for health checks: the failed read is retried on the replica.
  EXPECT_EQ(cl.command({"GET", k}), bulk("safe"));
  EXPECT_NE(cl.command({"INFO"}).find("read_failovers:"), std::string::npos);
}

TEST(Replication, HealthChecksMarkStoppedNodeDown) {
  Cluster c;
  EXPECT_EQ(c.proxy().node_state("node2"), "up");
  c.node(1).stop();
  EXPECT_TRUE(c.wait_state("node2", "down"));
  Client cl(c.port());
  EXPECT_NE(cl.command({"INFO"}).find("node2@127.0.0.1:" + std::to_string(c.node(1).port()) + ",state=down"),
            std::string::npos);
}

TEST(Replication, WritesAndReadsKeepWorkingWithOneCopyDown) {
  Cluster c;
  Client cl(c.port());
  c.node(1).stop();
  ASSERT_TRUE(c.wait_state("node2", "down"));
  for (const auto& k : keys_held_by(c.proxy(), "node2", "w")) {
    ASSERT_EQ(cl.command({"SET", k, "v"}), "+OK\r\n") << "write_quorum 1: one live copy is enough";
    ASSERT_EQ(cl.command({"GET", k}), bulk("v"));
  }
}

TEST(Replication, QuorumOfTwoRefusesWritesWithACopyDown) {
  Cluster c([](ProxyOptions& o) { o.write_quorum = 2; });
  Client cl(c.port());
  c.node(1).stop();
  ASSERT_TRUE(c.wait_state("node2", "down"));
  const auto on2 = keys_held_by(c.proxy(), "node2", "q");
  ASSERT_FALSE(on2.empty());
  EXPECT_EQ(cl.command({"SET", on2.front(), "v"}), "-ERR not enough replicas confirmed the write\r\n");
  for (int i = 0;; ++i) {  // a key with both copies on live nodes still works
    const std::string k = "q-live-" + std::to_string(i);
    if (!holds_copy(c.proxy(), k, "node2")) {
      EXPECT_EQ(cl.command({"SET", k, "v"}), "+OK\r\n");
      break;
    }
  }
}

TEST(Replication, CrashedNodeIsRefilledWhenItRestarts) {
  Cluster c;
  Client cl(c.port());
  write_keys(cl, "r", "before");
  c.restart(1);  // crash: comes back on the same port with an EMPTY cache
  // Detected through the changed run id, even without ever looking "down".
  ASSERT_TRUE(eventually([&] { return c.proxy().recoveries_completed() >= 1; }));
  ASSERT_EQ(c.proxy().node_state("node2"), "up");

  const auto held = keys_held_by(c.proxy(), "node2", "r");
  ASSERT_FALSE(held.empty());
  for (const auto& k : held) {
    ASSERT_EQ(c.node(1).cache.get(k), "before") << k << " was not copied back";
  }
  EXPECT_EQ(c.node(1).cache.size(), held.size()) << "refill copies only keys node2 owns";

  // The refilled copy now serves reads when the other copy is lost.
  for (std::size_t other = 0; other < c.size(); ++other) {
    if (other != 1) c.node(other).stop();
  }
  EXPECT_EQ(cl.command({"GET", held.front()}), bulk("before"));
}

TEST(Replication, PartitionedNodeIsWipedSoStaleDataNeverReturns) {
  Cluster c;
  Client cl(c.port());
  write_keys(cl, "p", "old");
  const auto held = keys_held_by(c.proxy(), "node2", "p");
  ASSERT_GE(held.size(), 2u);

  c.node(1).stop();  // cut off, but KEEPS its (soon outdated) data
  ASSERT_TRUE(c.wait_state("node2", "down"));
  std::vector<std::string> updated, deleted;
  for (std::size_t i = 0; i < held.size(); ++i) {
    if (i % 2 == 0) {
      ASSERT_EQ(cl.command({"SET", held[i], "new"}), "+OK\r\n");
      updated.push_back(held[i]);
    } else {
      ASSERT_EQ(cl.command({"DEL", held[i]}), ":1\r\n");
      deleted.push_back(held[i]);
    }
  }

  c.node(1).resume();  // back with stale values for every key above
  ASSERT_TRUE(eventually([&] { return c.proxy().recoveries_completed() >= 1; }));
  ASSERT_EQ(c.proxy().node_state("node2"), "up");
  for (const auto& k : updated) EXPECT_EQ(c.node(1).cache.get(k), "new") << k;
  for (const auto& k : deleted) {
    EXPECT_EQ(c.node(1).cache.get(k), std::nullopt) << k << " came back to life";
  }
  // And through the proxy, with node2 as the only copy left:
  for (std::size_t other = 0; other < c.size(); ++other) {
    if (other != 1) c.node(other).stop();
  }
  EXPECT_EQ(cl.command({"GET", updated.front()}), bulk("new"));
  EXPECT_EQ(cl.command({"GET", deleted.front()}), "$-1\r\n");
}

TEST(Replication, AddingANodeRebalancesWithoutLosingKeys) {
  Cluster c;
  Client cl(c.port());
  write_keys(cl, "a", "v");
  std::vector<NodeAddress> members;
  for (std::size_t i = 0; i < c.size(); ++i) members.push_back(c.address(i));
  members.push_back(c.add_node());
  c.proxy().set_nodes(members);
  ASSERT_TRUE(eventually([&] { return c.proxy().rebalances_completed() >= 1; }));

  // Every key is still readable, and every node holds exactly the keys it
  // owns under the new ring (new node filled, stale copies trimmed).
  const int n = cfg_int("test.proxy.replication_keys");
  for (int i = 0; i < n; ++i) ASSERT_EQ(cl.command({"GET", "a" + std::to_string(i)}), bulk("v"));
  std::size_t copies = 0;
  for (std::size_t node = 0; node < c.size(); ++node) {
    const auto held = keys_held_by(c.proxy(), Cluster::id(node), "a");
    EXPECT_EQ(c.node(node).cache.size(), held.size()) << Cluster::id(node);
    for (const auto& k : held) EXPECT_TRUE(c.node(node).cache.get(k).has_value()) << k;
    copies += c.node(node).cache.size();
  }
  EXPECT_EQ(copies, static_cast<std::size_t>(n) * kRf);
  EXPECT_GT(c.node(3).cache.size(), 0u) << "the new node received its share";
}

TEST(Replication, RemovingANodeKeepsEveryKeyReadable) {
  Cluster c;
  Client cl(c.port());
  write_keys(cl, "x", "v");
  c.proxy().set_nodes({c.address(0), c.address(1)});
  ASSERT_TRUE(eventually([&] { return c.proxy().rebalances_completed() >= 1; }));
  c.node(2).stop();  // gone for good
  const int n = cfg_int("test.proxy.replication_keys");
  for (int i = 0; i < n; ++i) ASSERT_EQ(cl.command({"GET", "x" + std::to_string(i)}), bulk("v"));
}

TEST(Replication, DeleteWritesTombstonesOnEveryCopy) {
  Cluster c;
  Client cl(c.port());
  ASSERT_EQ(cl.command({"SET", "gone", "v"}), "+OK\r\n");
  ASSERT_EQ(cl.command({"DEL", "gone"}), ":1\r\n");
  for (const auto& o : c.proxy().owners_of("gone")) {
    const auto e = find_entry(c.cache_of(o), "gone");
    ASSERT_TRUE(e.has_value()) << o;
    EXPECT_TRUE(e->tombstone) << o;
  }
  EXPECT_EQ(cl.command({"GET", "gone"}), "$-1\r\n");
  EXPECT_EQ(cl.command({"DBSIZE"}), ":0\r\n") << "tombstones aren't counted as keys";
}
