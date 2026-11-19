#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cachegrid/LRUCache.h"  // Millis
#include "cachegrid/Net.h"

namespace cachegrid {

class Config;
class NodeClient;

// A cache node as the proxy sees it. The id (not the address) is what gets
// hashed onto the ring, so a node can move to a new address without its keys
// moving to other nodes.
struct NodeAddress {
  std::string id;
  std::string host;
  std::uint16_t port;

  std::string to_string() const { return id + "@" + host + ":" + std::to_string(port); }
  bool operator==(const NodeAddress& o) const { return id == o.id && host == o.host && port == o.port; }
};

// Health of a node, as judged by the proxy.
enum class NodeState : int {
  Up,          // healthy: serves reads and writes
  Down,        // failed health checks: skipped for writes, last resort for reads
  Recovering,  // back, but being refilled from its peers: gets writes, not reads
};
const char* to_string(NodeState s);

// All values come from the `proxy.*` keys in config/proxy.conf.
struct ProxyOptions {
  ListenerOptions net;
  std::vector<NodeAddress> nodes;      // cluster membership
  std::size_t vnodes_per_node;         // points per node on the hash ring
  Millis connect_timeout;              // give up connecting to a node after this
  Millis request_timeout;              // node silent this long = treated as failed
  Millis reconnect_backoff;            // after a failure, fail fast for this long
  Millis timer_tick;                   // how often timeouts are checked
  std::size_t max_pending_per_client;  // in-flight commands per client before pausing reads
  std::size_t max_reply_bytes;         // largest reply accepted from a node

  // Replication (Phase 5)
  std::uint8_t proxy_id;               // makes this proxy's write versions unique
  std::size_t replication_factor;      // copies of each key
  std::size_t write_quorum;            // copies that must confirm a write
  Millis health_interval;              // time between health-check rounds
  Millis health_timeout;               // a PING slower than this counts as failed
  std::size_t health_fail_threshold;   // consecutive failed PINGs before "Down"
  Millis recovery_grace;               // wait before copying data to a recovering node
  std::size_t resync_batch;            // commands per round trip when copying data
  bool log_events;                     // print health/recovery events to stdout

  static ProxyOptions from_config(const Config& cfg);

  // Parses "id@host:port" items. Throws ConfigError on bad or duplicate entries.
  static std::vector<NodeAddress> parse_nodes(const std::vector<std::string>& items);
};

// The routing layer in front of the cache nodes.
//
// Clients connect to the proxy exactly as they would to a node (same RESP
// protocol). Every key lives on `replication_factor` nodes: its owner on the
// consistent-hash ring plus the next distinct nodes clockwise.
//
//   GET key            -> healthiest copy; on failure, retried on the next copy
//   SET key ...        -> written to every copy with a fresh version number;
//                         OK once `write_quorum` copies confirm
//   DEL k1 k2 ...      -> versioned delete (tombstone) on every copy of each key
//   DBSIZE             -> summed over reachable nodes (counts every copy)
//   PING/INFO/QUIT/COMMAND -> answered by the proxy itself
//
// A background health thread PINGs every node. A node that fails
// `health_fail_threshold` checks in a row is marked Down. When it answers
// again it is wiped (it may hold outdated data), refilled from its peers and
// only then marked Up. After a membership change every node is refilled with
// the keys it now owns and trimmed of the keys it no longer owns.
//
// Consistency policy: last writer wins by version. Versions are this proxy's
// clock in microseconds plus `proxy_id`, so they're unique across proxies;
// with several proxies, clock skew between their machines decides which of
// two near-simultaneous writes wins.
class ProxyServer {
 public:
  explicit ProxyServer(ProxyOptions opts);
  ~ProxyServer();
  ProxyServer(const ProxyServer&) = delete;
  ProxyServer& operator=(const ProxyServer&) = delete;

  void start();
  void stop();

  std::uint16_t port() const { return bound_port_; }
  std::size_t active_connections() const { return active_.load(); }

  // Replaces cluster membership (e.g. after a config reload). Thread-safe.
  // New nodes start as Recovering; the health thread then rebalances data.
  // Throws std::invalid_argument if `nodes` is empty.
  void set_nodes(std::vector<NodeAddress> nodes);

  // Id of the node that owns `key` / all nodes holding a copy (owner first).
  std::string owner_of(std::string_view key) const;
  std::vector<std::string> owners_of(std::string_view key) const;

  // Health state of a node by id ("unknown" if not a member).
  std::string node_state(const std::string& node_id) const;

  // Completed recoveries and rebalances (for tests and INFO).
  std::uint64_t recoveries_completed() const { return recoveries_.load(); }
  std::uint64_t rebalances_completed() const { return rebalances_.load(); }

  // A fresh, unique, increasing write version.
  std::uint64_t next_version();

 private:
  struct Membership;
  class Worker;
  friend class Worker;
  using Clients = std::map<std::string, std::unique_ptr<NodeClient>>;  // key: NodeAddress::to_string()

  void append_info(std::string& out) const;
  std::shared_ptr<const Membership> membership() const;

  // Health / recovery (background thread).
  void health_loop();
  NodeClient& client_for(Clients& clients, const NodeAddress& a);
  void recover(const Membership& m, std::size_t node, Clients& clients);
  void rebalance(const Membership& m, Clients& clients);
  bool fill(const Membership& m, std::size_t target, Clients& clients);
  bool trim(const Membership& m, std::size_t node, Clients& clients);
  bool sleep_unless_stopping(Millis d);
  void log_event(const std::string& msg) const;

  ProxyOptions opts_;
  int listen_fd_ = -1;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stopping_{false};
  bool started_ = false;

  // Membership is read on every command by every worker. Instead of locking
  // for each read, workers keep their own copy and only take the lock when
  // `membership_version_` says it changed (rare: config reloads).
  mutable std::mutex membership_mu_;
  std::shared_ptr<const Membership> membership_;
  std::atomic<std::uint64_t> membership_version_{0};

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;

  std::thread health_thread_;
  std::mutex health_mu_;
  std::condition_variable health_cv_;
  std::atomic<bool> rebalance_requested_{false};

  std::atomic<std::uint64_t> last_version_{0};

  std::atomic<std::size_t> active_{0};
  std::atomic<std::uint64_t> total_accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> commands_{0};
  std::atomic<std::uint64_t> forwarded_{0};
  std::atomic<std::uint64_t> node_failures_{0};
  std::atomic<std::uint64_t> failovers_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
  std::atomic<std::uint64_t> recoveries_{0};
  std::atomic<std::uint64_t> rebalances_{0};
};

}  // namespace cachegrid
