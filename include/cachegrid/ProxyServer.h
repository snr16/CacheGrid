#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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

  static ProxyOptions from_config(const Config& cfg);

  // Parses "id@host:port" items. Throws ConfigError on bad or duplicate entries.
  static std::vector<NodeAddress> parse_nodes(const std::vector<std::string>& items);
};

// The routing layer in front of the cache nodes.
//
// Clients connect to the proxy exactly as they would to a node (same RESP
// protocol). For each command the proxy hashes the key onto the consistent
// hash ring, forwards the command to the owning node, and relays the reply.
//
//   GET/SET key ...      -> forwarded to the key's owner
//   DEL k1 k2 ...        -> split per owner, sent in parallel, counts summed
//   DBSIZE               -> sent to every node, results summed
//   PING/INFO/QUIT/COMMAND -> answered by the proxy itself
//
// Everything is asynchronous: one worker thread multiplexes many clients and
// one pipelined connection per node, never blocking on a node's reply.
// Replies are always returned to each client in the order it sent commands,
// even when they come from different nodes.
//
// The proxy holds no cache data, so several can run side by side.
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
  // Commands already sent finish on their old node; new commands use the new
  // ring. Throws std::invalid_argument if `nodes` is empty.
  void set_nodes(std::vector<NodeAddress> nodes);

  // Id of the node that owns `key` under the current membership.
  std::string owner_of(std::string_view key) const;

 private:
  struct Membership;
  class Worker;
  friend class Worker;

  void append_info(std::string& out) const;
  std::shared_ptr<const Membership> membership() const;

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

  std::atomic<std::size_t> active_{0};
  std::atomic<std::uint64_t> total_accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> commands_{0};
  std::atomic<std::uint64_t> forwarded_{0};
  std::atomic<std::uint64_t> node_failures_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
};

}  // namespace cachegrid
