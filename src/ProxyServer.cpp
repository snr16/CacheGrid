#include "cachegrid/ProxyServer.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "cachegrid/CommandHandler.h"  // scan_entry layout
#include "cachegrid/Config.h"
#include "cachegrid/HashRing.h"
#include "cachegrid/NodeClient.h"
#include "cachegrid/Poller.h"
#include "cachegrid/ProcessStats.h"
#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

// Case-insensitive equality with an upper-case literal, without allocating
// (this runs for every command).
bool is_cmd(const std::string& arg, std::string_view upper_name) {
  if (arg.size() != upper_name.size()) return false;
  for (std::size_t i = 0; i < arg.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(arg[i])) != upper_name[i]) return false;
  }
  return true;
}

std::string arity_error(const std::string& cmd) {
  std::string name = cmd;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return "-ERR wrong number of arguments for '" + name + "' command\r\n";
}

std::string unavailable(const NodeAddress& n) {
  return "-ERR node " + n.id + " (" + n.host + ":" + std::to_string(n.port) + ") unavailable\r\n";
}

const std::string kOk = "+OK\r\n";
const std::string kStale = "+STALE\r\n";
const std::string kNoQuorum = "-ERR not enough replicas confirmed the write\r\n";

}  // namespace

const char* to_string(NodeState s) {
  switch (s) {
    case NodeState::Up: return "up";
    case NodeState::Down: return "down";
    case NodeState::Recovering: return "recovering";
  }
  return "unknown";
}

// ---- Options -------------------------------------------------------------------

std::vector<NodeAddress> ProxyOptions::parse_nodes(const std::vector<std::string>& items) {
  std::vector<NodeAddress> nodes;
  std::unordered_set<std::string> ids;
  for (const auto& item : items) {
    const auto at = item.find('@');
    const auto colon = item.rfind(':');
    if (at == std::string::npos || colon == std::string::npos || colon < at) {
      throw ConfigError("'proxy.nodes' entry '" + item + "' must look like id@host:port");
    }
    NodeAddress n;
    n.id = item.substr(0, at);
    n.host = item.substr(at + 1, colon - at - 1);
    const std::string port = item.substr(colon + 1);
    char* end = nullptr;
    const long p = std::strtol(port.c_str(), &end, 10);
    if (n.id.empty() || n.host.empty() || port.empty() || *end != '\0' || p < 1 || p > 65535) {
      throw ConfigError("'proxy.nodes' entry '" + item + "' must look like id@host:port");
    }
    n.port = static_cast<std::uint16_t>(p);
    if (!ids.insert(n.id).second) throw ConfigError("'proxy.nodes' has duplicate node id '" + n.id + "'");
    nodes.push_back(std::move(n));
  }
  if (nodes.empty()) throw ConfigError("'proxy.nodes' must list at least one node");
  return nodes;
}

ProxyOptions ProxyOptions::from_config(const Config& cfg) {
  ProxyOptions o{};
  o.nodes = parse_nodes(cfg.get_list("proxy.nodes"));
  const std::size_t workers = cfg.get_size("proxy.worker_threads");
  // Besides the usual listener fds: one connection per node per worker, plus
  // one per node for the health thread.
  const std::size_t internal =
      ListenerOptions::base_internal_fds(workers) + (workers + 1) * o.nodes.size();
  o.net = ListenerOptions::from_config(cfg, "proxy", internal);

  o.vnodes_per_node = cfg.get_size("proxy.vnodes_per_node");
  o.connect_timeout = Millis(cfg.get_size("proxy.connect_timeout_ms"));
  o.request_timeout = Millis(cfg.get_size("proxy.request_timeout_ms"));
  o.reconnect_backoff = Millis(cfg.get_size("proxy.reconnect_backoff_ms"));
  o.timer_tick = Millis(cfg.get_size("proxy.timer_tick_ms"));
  o.max_pending_per_client = cfg.get_size("proxy.max_pending_per_client");
  o.max_reply_bytes = cfg.get_bytes("proxy.max_reply_bytes");

  const std::size_t id = cfg.get_size("proxy.id");
  if (id > std::numeric_limits<std::uint8_t>::max()) throw ConfigError("'proxy.id' must be 0..255");
  o.proxy_id = static_cast<std::uint8_t>(id);
  o.replication_factor = cfg.get_size("proxy.replication_factor");
  o.write_quorum = cfg.get_size("proxy.write_quorum");
  o.health_interval = Millis(cfg.get_size("proxy.health_interval_ms"));
  o.health_timeout = Millis(cfg.get_size("proxy.health_timeout_ms"));
  o.health_fail_threshold = cfg.get_size("proxy.health_fail_threshold");
  o.recovery_grace = Millis(cfg.get_size("proxy.recovery_grace_ms"));
  o.resync_batch = cfg.get_size("proxy.resync_batch");
  o.log_events = cfg.get_bool("proxy.log_events");

  const std::pair<std::size_t, const char*> positives[] = {
      {o.vnodes_per_node, "proxy.vnodes_per_node"},
      {static_cast<std::size_t>(o.connect_timeout.count()), "proxy.connect_timeout_ms"},
      {static_cast<std::size_t>(o.request_timeout.count()), "proxy.request_timeout_ms"},
      {static_cast<std::size_t>(o.timer_tick.count()), "proxy.timer_tick_ms"},
      {o.max_pending_per_client, "proxy.max_pending_per_client"},
      {o.max_reply_bytes, "proxy.max_reply_bytes"},
      {o.replication_factor, "proxy.replication_factor"},
      {o.write_quorum, "proxy.write_quorum"},
      {static_cast<std::size_t>(o.health_interval.count()), "proxy.health_interval_ms"},
      {static_cast<std::size_t>(o.health_timeout.count()), "proxy.health_timeout_ms"},
      {o.health_fail_threshold, "proxy.health_fail_threshold"},
      {o.resync_batch, "proxy.resync_batch"},
  };
  for (const auto& [v, key] : positives) {
    if (v == 0) throw ConfigError(std::string("'") + key + "' must be >= 1");
  }
  if (o.timer_tick.count() > std::numeric_limits<int>::max()) {
    throw ConfigError("'proxy.timer_tick_ms' is too large");
  }
  if (o.write_quorum > o.replication_factor) {
    throw ConfigError("'proxy.write_quorum' must be <= 'proxy.replication_factor'");
  }
  // Writes that were already on their way before a node switched to
  // Recovering finish (or time out) within request_timeout. Waiting at least
  // that long before copying data guarantees none of them is missed.
  if (o.recovery_grace < o.request_timeout) {
    throw ConfigError("'proxy.recovery_grace_ms' must be >= 'proxy.request_timeout_ms'");
  }
  return o;
}

// ---- Membership ------------------------------------------------------------------

struct ProxyServer::Membership {
  Membership(std::vector<NodeAddress> n, std::size_t vnodes, std::size_t replication)
      : nodes(std::move(n)),
        ring(vnodes),
        rf(replication),
        state(std::make_unique<std::atomic<int>[]>(nodes.size())) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      ring.add_node(nodes[i].id);
      index.emplace(nodes[i].id, i);
      state[i].store(static_cast<int>(NodeState::Up));
    }
    // The ring numbers nodes in sorted-id order; map that to our order.
    for (const auto& id : ring.nodes()) ring_to_node.push_back(static_cast<std::uint32_t>(index.at(id)));
  }

  std::size_t owner(std::string_view key) const { return index.at(ring.owner(key)); }

  // Hot path: owners as positions in `nodes`, written into a reused buffer.
  void owners_into(std::string_view key, std::vector<std::uint32_t>& out) const {
    ring.owner_positions(key, rf, out);
    for (auto& p : out) p = ring_to_node[p];
  }

  std::vector<std::size_t> owners(std::string_view key) const {
    std::vector<std::uint32_t> pos;
    owners_into(key, pos);
    return std::vector<std::size_t>(pos.begin(), pos.end());
  }

  bool owns(std::size_t node, std::string_view key) const {
    const auto o = owners(key);
    return std::find(o.begin(), o.end(), node) != o.end();
  }

  NodeState state_of(std::size_t i) const { return static_cast<NodeState>(state[i].load()); }
  void set_state(std::size_t i, NodeState s) const { state[i].store(static_cast<int>(s)); }

  std::vector<NodeAddress> nodes;
  HashRing ring;
  std::size_t rf;
  std::unordered_map<std::string, std::size_t> index;  // node id -> position in `nodes`
  std::vector<std::uint32_t> ring_to_node;             // ring position -> position in `nodes`
  // Health state per node. Written by the health thread, read by workers on
  // every request: atomics, no lock.
  std::unique_ptr<std::atomic<int>[]> state;
};

// ---- Worker: one event-loop thread ---------------------------------------------------

class ProxyServer::Worker {
 public:
  explicit Worker(ProxyServer& proxy)
      : proxy_(proxy),
        read_buf_(proxy.opts_.net.read_chunk_bytes),
        limits_{proxy.opts_.net.max_request_bytes, proxy.opts_.net.max_args} {
    int fds[2];
    if (::pipe(fds) < 0) throw std::system_error(errno, std::generic_category(), "pipe");
    wake_r_ = fds[0];
    wake_w_ = fds[1];
    net::set_nonblocking(wake_r_);
    net::set_nonblocking(wake_w_);
    poller_.add_readonly(wake_r_, false);
    poller_.add_readonly(proxy.listen_fd_, true);
  }

  ~Worker() {
    for (auto& [fd, id] : fd_client_) ::close(fd);
    for (auto& [key, b] : backends_) {
      if (b->fd >= 0) ::close(b->fd);
    }
    ::close(wake_r_);
    ::close(wake_w_);
  }

  void wake() {
    const char b = 1;
    [[maybe_unused]] auto n = ::write(wake_w_, &b, 1);
  }

  void run() {
    std::vector<PollEvent> events;
    const int max_events = static_cast<int>(proxy_.opts_.net.events_per_poll);
    const int tick_ms = static_cast<int>(proxy_.opts_.timer_tick.count());
    while (!proxy_.stopping_.load()) {
      const int n = poller_.wait(events, max_events, tick_ms);
      for (int i = 0; i < n; ++i) handle(events[static_cast<std::size_t>(i)]);
      check_timeouts();
      settle();
    }
    for (auto& [fd, id] : fd_client_) {
      ::close(fd);
      proxy_.active_.fetch_sub(1);
    }
    fd_client_.clear();
    clients_.clear();
  }

 private:
  using TimePoint = Clock::time_point;

  // One reply position in a client's ordered output.
  struct Slot {
    enum class Kind {
      Simple,  // one node reply, relayed as-is (or a local reply)
      Read,    // GET: try copies in order until one answers
      Write,   // SET: sent to every copy, OK once `quorum` confirm
      Delete,  // DEL: per key, sent to every copy; sum of keys removed
      Sum,     // DBSIZE: integers summed over nodes
    };
    Kind kind = Kind::Simple;
    bool done = false;
    std::string reply;

    std::size_t remaining = 0;  // node replies still expected (Write/Delete/Sum)
    std::string error;          // first error seen
    std::int64_t sum = 0;       // Sum
    std::size_t quorum = 0;     // Write/Delete: confirmations needed
    std::size_t acks = 0;       // Write: confirmations received

    struct KeyResult {
      std::size_t acks = 0;
      bool removed = false;
    };
    std::vector<KeyResult> keys;  // Delete: one per key

    std::size_t next_candidate = 0;  // Read: which copy to try next on failover
    std::string request;             // Read: re-sent on failover
  };

  struct Client {
    int fd = -1;
    std::string in;
    std::string out;
    std::deque<Slot> slots;       // oldest first
    std::uint64_t first_seq = 0;  // sequence number of slots.front()
    bool closing = false;
    bool watch_read = true;
    bool watch_write = false;
  };

  // A command sent to a node, waiting for its reply.
  struct Pending {
    std::uint64_t client_id;
    std::uint64_t seq;
    std::uint32_t part;  // which key, for multi-key DEL
    TimePoint queued_at;
  };

  // This worker's connection to one cache node. Commands from all of the
  // worker's clients are pipelined over it; a node answers in order, so the
  // n-th reply belongs to the n-th entry in `pending`.
  struct Backend {
    enum class State { Idle, Connecting, Connected };
    NodeAddress addr;
    State state = State::Idle;
    int fd = -1;
    std::string in;
    std::string out;
    std::deque<Pending> pending;
    TimePoint connect_started{};
    TimePoint failed_at{};
    bool has_failed = false;
    bool watch_write = false;
    bool dirty = false;  // queued for flushing in settle()
  };

  // ---- event dispatch ----

  void handle(const PollEvent& ev) {
    if (ev.fd == wake_r_) {
      // Empty the pipe (reusing the configured read buffer).
      while (::read(wake_r_, read_buf_.data(), read_buf_.size()) > 0) {
      }
      return;
    }
    if (ev.fd == proxy_.listen_fd_) {
      accept_all();
      return;
    }
    if (auto c = fd_client_.find(ev.fd); c != fd_client_.end()) {
      on_client_event(c->second, ev);
      return;
    }
    if (auto b = fd_backend_.find(ev.fd); b != fd_backend_.end()) {
      on_backend_event(*b->second, ev);
    }
  }

  // After events: process clients and flush nodes until nothing changes.
  // All work funnels through here, so slot queues are only reshaped in one
  // place and nothing re-enters a half-finished operation.
  void settle() {
    while (!dirty_clients_.empty() || !dirty_backends_.empty()) {
      while (!dirty_clients_.empty()) {
        const std::uint64_t id = dirty_clients_.back();
        dirty_clients_.pop_back();
        settle_client(id);
      }
      flushing_.swap(dirty_backends_);
      for (Backend* b : flushing_) {
        b->dirty = false;
        flush_backend(*b);
      }
      flushing_.clear();
    }
  }

  // ---- clients ----

  void accept_all() {
    for (;;) {
      const int fd = ::accept(proxy_.listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (!net::would_block()) std::perror("accept");
        return;
      }
      proxy_.total_accepted_.fetch_add(1);
      if (!net::set_nonblocking(fd)) {
        ::close(fd);
        continue;
      }
      net::configure_stream_socket(fd);
      if (proxy_.active_.fetch_add(1) >= proxy_.opts_.net.max_connections) {
        proxy_.active_.fetch_sub(1);
        proxy_.rejected_.fetch_add(1);
        static constexpr std::string_view kMsg = "-ERR max number of clients reached\r\n";
        [[maybe_unused]] auto n = ::send(fd, kMsg.data(), kMsg.size(), net::kSendFlags);
        ::close(fd);
        continue;
      }
      const std::uint64_t id = next_client_id_++;
      poller_.add(fd);
      fd_client_.emplace(fd, id);
      Client c;
      c.fd = fd;
      clients_.emplace(id, std::move(c));
    }
  }

  void on_client_event(std::uint64_t id, const PollEvent& ev) {
    Client& c = clients_.at(id);
    if (ev.readable && !c.closing) {
      ssize_t n;
      do {
        n = ::recv(c.fd, read_buf_.data(), read_buf_.size(), 0);
      } while (n < 0 && errno == EINTR);
      if (n == 0) {
        c.closing = true;  // finish what was sent, then close
      } else if (n < 0 && !net::would_block()) {
        close_client(id);
        return;
      } else if (n > 0) {
        c.in.append(read_buf_.data(), static_cast<std::size_t>(n));
      }
    }
    mark_dirty(id);
  }

  void settle_client(std::uint64_t id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    Client& c = it->second;

    // Release replies finished since the last visit FIRST: otherwise a queue
    // full of finished-but-unreleased replies would look "full" to process(),
    // and buffered commands would wait forever (no new event would wake us).
    pump(c);
    process(id, c);
    pump(c);
    if (c.out.size() > proxy_.opts_.net.max_output_buffer || !flush_client(c)) {
      close_client(id);
      return;
    }
    if (c.closing && c.slots.empty() && c.out.empty()) {
      close_client(id);
      return;
    }
    // Pause reading while too many commands are in flight (back-pressure);
    // stop reading entirely once closing.
    const bool read = !c.closing && c.slots.size() < proxy_.opts_.max_pending_per_client;
    const bool write = !c.out.empty();
    if (read != c.watch_read || write != c.watch_write) {
      poller_.set_interest(c.fd, read, write);
      c.watch_read = read;
      c.watch_write = write;
    }
  }

  void process(std::uint64_t id, Client& c) {
    std::size_t off = 0;
    while (!c.closing && c.slots.size() < proxy_.opts_.max_pending_per_client) {
      resp::ParseResult r = resp::parse_command(std::string_view(c.in).substr(off), limits_);
      if (r.status == resp::ParseStatus::Incomplete) break;
      if (r.status == resp::ParseStatus::Error) {
        proxy_.protocol_errors_.fetch_add(1);
        push_done(c, "-ERR Protocol error: " + r.error + "\r\n");
        c.closing = true;
        break;
      }
      off += r.consumed;
      if (r.args.empty()) continue;
      proxy_.commands_.fetch_add(1);
      dispatch(id, c, r.args);
      // Release finished replies right away, so only commands genuinely
      // waiting on a node count toward max_pending_per_client.
      pump(c);
    }
    c.in.erase(0, off);
  }

  void dispatch(std::uint64_t id, Client& c, const std::vector<std::string>& args) {
    refresh_membership();
    const std::size_t argc = args.size();
    const std::string& cmd = args[0];

    if (is_cmd(cmd, "GET")) {
      if (argc != 2) {
        push_done(c, arity_error(cmd));
        return;
      }
      m_->owners_into(args[1], owners_buf_);
      read_order_into(owners_buf_, targets_buf_);
      Slot s;
      s.kind = Slot::Kind::Read;
      s.next_candidate = 1;
      s.request = resp::encode_command(args);
      const std::uint64_t seq = push_slot(c, std::move(s));
      forward(*targets_buf_.front(), c.slots.back().request, id, seq, 0);
    } else if (is_cmd(cmd, "SET")) {
      if (argc != 3 && argc != 5) {
        push_done(c, arity_error(cmd));
        return;
      }
      // VSET <version> key value [EX|PX n], encoded straight into one buffer.
      request_buf_.clear();
      resp::write_array_header(request_buf_, argc + 1);
      resp::write_bulk(request_buf_, "VSET");
      resp::write_bulk(request_buf_, std::to_string(proxy_.next_version()));
      for (std::size_t i = 1; i < argc; ++i) resp::write_bulk(request_buf_, args[i]);
      m_->owners_into(args[1], owners_buf_);
      write_targets_into(owners_buf_, targets_buf_);
      Slot s;
      s.kind = Slot::Kind::Write;
      s.quorum = std::min(proxy_.opts_.write_quorum, owners_buf_.size());
      s.remaining = targets_buf_.size();
      const std::uint64_t seq = push_slot(c, std::move(s));
      for (Backend* b : targets_buf_) forward(*b, request_buf_, id, seq, 0);
    } else if (is_cmd(cmd, "DEL")) {
      if (argc < 2) {
        push_done(c, arity_error(cmd));
        return;
      }
      const std::string version = std::to_string(proxy_.next_version());
      // Pass 1: pick every key's targets (one snapshot of node states), so the
      // slot knows exactly how many replies to expect before anything is sent.
      std::vector<Backend*>& all_targets = failover_buf_;
      all_targets.clear();
      per_key_targets_.clear();
      for (std::size_t i = 1; i < argc; ++i) {
        m_->owners_into(args[i], owners_buf_);
        write_targets_into(owners_buf_, targets_buf_);
        all_targets.insert(all_targets.end(), targets_buf_.begin(), targets_buf_.end());
        per_key_targets_.push_back(static_cast<std::uint32_t>(targets_buf_.size()));
      }
      Slot s;
      s.kind = Slot::Kind::Delete;
      // A key can't have more copies than there are nodes.
      s.quorum = std::min({proxy_.opts_.write_quorum, m_->rf, m_->nodes.size()});
      s.remaining = all_targets.size();
      s.keys.resize(argc - 1);
      const std::uint64_t seq = push_slot(c, std::move(s));
      // Pass 2: send. (Copy the target list: a synchronous failure reply can
      // trigger a read failover elsewhere that reuses failover_buf_.)
      const std::vector<Backend*> targets = all_targets;
      std::size_t t = 0;
      for (std::size_t i = 1; i < argc; ++i) {
        request_buf_.clear();
        resp::write_array_header(request_buf_, 3);
        resp::write_bulk(request_buf_, "VDEL");
        resp::write_bulk(request_buf_, version);
        resp::write_bulk(request_buf_, args[i]);
        for (std::uint32_t k = 0; k < per_key_targets_[i - 1]; ++k) {
          forward(*targets[t++], request_buf_, id, seq, static_cast<std::uint32_t>(i - 1));
        }
      }
    } else if (is_cmd(cmd, "DBSIZE")) {
      std::vector<Backend*> nodes;
      for (std::size_t n = 0; n < m_->nodes.size(); ++n) {
        if (m_->state_of(n) != NodeState::Down) nodes.push_back(node_backend_[n]);
      }
      if (nodes.empty()) {
        push_done(c, ":0\r\n");
        return;
      }
      Slot s;
      s.kind = Slot::Kind::Sum;
      s.remaining = nodes.size();
      const std::uint64_t seq = push_slot(c, std::move(s));
      const std::string req = resp::encode_command({"DBSIZE"});
      for (Backend* b : nodes) forward(*b, req, id, seq, 0);
    } else if (is_cmd(cmd, "PING")) {
      if (argc == 1) {
        push_done(c, "+PONG\r\n");
      } else if (argc == 2) {
        std::string r;
        resp::write_bulk(r, args[1]);
        push_done(c, r);
      } else {
        push_done(c, arity_error(cmd));
      }
    } else if (is_cmd(cmd, "INFO")) {
      std::string text;
      proxy_.append_info(text);
      std::string r;
      resp::write_bulk(r, text);
      push_done(c, r);
    } else if (is_cmd(cmd, "QUIT")) {
      push_done(c, kOk);
      c.closing = true;
    } else if (is_cmd(cmd, "COMMAND")) {
      push_done(c, "*0\r\n");
    } else {
      push_done(c, "-ERR unknown command '" + args[0] + "'\r\n");
    }
  }

  // Copies to read from, best first: Up, then Recovering (may lack data), then
  // Down (last resort: it may have come back before the health check noticed).
  void read_order_into(const std::vector<std::uint32_t>& owners, std::vector<Backend*>& out) const {
    out.clear();
    for (NodeState wanted : {NodeState::Up, NodeState::Recovering, NodeState::Down}) {
      for (std::uint32_t n : owners) {
        if (m_->state_of(n) == wanted) out.push_back(node_backend_[n]);
      }
    }
  }

  // Copies to write to: every copy not known to be Down (a Down node is
  // refilled when it recovers). If all are Down, try them anyway.
  void write_targets_into(const std::vector<std::uint32_t>& owners, std::vector<Backend*>& out) const {
    out.clear();
    for (std::uint32_t n : owners) {
      if (m_->state_of(n) != NodeState::Down) out.push_back(node_backend_[n]);
    }
    if (out.empty()) {
      for (std::uint32_t n : owners) out.push_back(node_backend_[n]);
    }
  }

  std::uint64_t push_slot(Client& c, Slot s) {
    const std::uint64_t seq = c.first_seq + c.slots.size();
    c.slots.push_back(std::move(s));
    return seq;
  }

  // A reply the proxy produces itself. It still takes a slot, so it can't
  // overtake earlier commands that are waiting on a node.
  void push_done(Client& c, std::string reply) {
    Slot s;
    s.done = true;
    s.reply = std::move(reply);
    push_slot(c, std::move(s));
  }

  // Moves finished replies from the front of the slot queue to the output.
  static void pump(Client& c) {
    while (!c.slots.empty() && c.slots.front().done) {
      c.out += c.slots.front().reply;
      c.slots.pop_front();
      ++c.first_seq;
    }
  }

  // A node's reply for slot `seq`, or (failed = true) an error standing in for
  // a reply because the node couldn't be reached.
  void deliver(std::uint64_t client_id, std::uint64_t seq, std::uint32_t part, std::string_view reply,
               bool failed) {
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;  // client left; drop the reply
    Client& c = it->second;
    if (seq < c.first_seq || seq - c.first_seq >= c.slots.size()) return;
    Slot& s = c.slots[static_cast<std::size_t>(seq - c.first_seq)];
    mark_dirty(client_id);

    switch (s.kind) {
      case Slot::Kind::Simple:
        s.reply.assign(reply);
        s.done = true;
        break;

      case Slot::Kind::Read:
        if (!failed) {
          s.reply.assign(reply);
          s.done = true;
        } else if (Backend* next = failover_target(s)) {
          // Failover: try the next copy. (Nothing touches `s` after this
          // call, since forward() may deliver again synchronously.)
          proxy_.failovers_.fetch_add(1);
          forward(*next, s.request, client_id, seq, 0);
        } else {
          s.reply.assign(reply);  // every copy failed
          s.done = true;
        }
        break;

      case Slot::Kind::Write:
        if (!failed && (reply == kOk || reply == kStale)) {
          ++s.acks;
        } else if (s.error.empty()) {
          s.error.assign(reply);
        }
        if (--s.remaining == 0) {
          s.done = true;
          s.reply = s.acks >= s.quorum ? kOk : (s.error.empty() ? kNoQuorum : s.error);
        }
        break;

      case Slot::Kind::Delete: {
        auto& k = s.keys[part];
        if (!failed && !reply.empty() && reply[0] == ':') {
          ++k.acks;
          if (reply == ":1\r\n") k.removed = true;
        } else if (s.error.empty()) {
          s.error.assign(reply);
        }
        if (--s.remaining == 0) {
          s.done = true;
          std::int64_t removed = 0;
          bool all_confirmed = true;
          for (const auto& key : s.keys) {
            all_confirmed = all_confirmed && key.acks >= s.quorum;
            removed += key.removed ? 1 : 0;
          }
          s.reply = all_confirmed ? ":" + std::to_string(removed) + "\r\n"
                                  : (s.error.empty() ? kNoQuorum : s.error);
        }
        break;
      }

      case Slot::Kind::Sum:
        if (!failed && !reply.empty() && reply[0] == ':') {
          s.sum += std::strtoll(std::string(reply.substr(1)).c_str(), nullptr, 10);
        } else if (s.error.empty()) {
          s.error.assign(reply);
        }
        if (--s.remaining == 0) {
          s.done = true;
          s.reply = s.error.empty() ? ":" + std::to_string(s.sum) + "\r\n" : s.error;
        }
        break;
    }
  }

  // Next copy to try for a failed read, or nullptr. Rare path, so it simply
  // recomputes the key's copies (the key is recovered from the stored request).
  Backend* failover_target(Slot& s) {
    resp::ParseResult r = resp::parse_command(s.request, limits_);
    if (r.status != resp::ParseStatus::Ok || r.args.size() != 2) return nullptr;
    m_->owners_into(r.args[1], owners_buf_);
    read_order_into(owners_buf_, failover_buf_);
    if (s.next_candidate >= failover_buf_.size()) return nullptr;
    return failover_buf_[s.next_candidate++];
  }

  bool flush_client(Client& c) {
    while (!c.out.empty()) {
      const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), net::kSendFlags);
      if (n > 0) {
        c.out.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      return n < 0 && net::would_block();
    }
    return true;
  }

  void close_client(std::uint64_t id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    poller_.remove(it->second.fd);
    ::close(it->second.fd);
    fd_client_.erase(it->second.fd);
    clients_.erase(it);
    proxy_.active_.fetch_sub(1);
    // Replies still on their way from nodes for this client will find no
    // client in deliver() and be dropped.
  }

  void mark_dirty(std::uint64_t id) { dirty_clients_.push_back(id); }

  // ---- nodes ----

  void refresh_membership() {
    const std::uint64_t v = proxy_.membership_version_.load(std::memory_order_acquire);
    if (v != m_version_ || !m_) {
      m_ = proxy_.membership();
      m_version_ = v;
      node_backend_.clear();
      for (const auto& addr : m_->nodes) node_backend_.push_back(&backend_for(addr));
    }
  }

  Backend& backend_for(const NodeAddress& addr) {
    auto& slot = backends_[addr.to_string()];
    if (!slot) {
      slot = std::make_unique<Backend>();
      slot->addr = addr;
    }
    return *slot;
  }

  void mark_dirty(Backend& b) {
    if (!b.dirty) {
      b.dirty = true;
      dirty_backends_.push_back(&b);
    }
  }

  void forward(Backend& b, std::string_view request, std::uint64_t client_id, std::uint64_t seq,
               std::uint32_t part) {
    const TimePoint now = Clock::now();

    if (b.state == Backend::State::Idle) {
      // Recently failed: answer immediately instead of making every request
      // wait out a connect timeout against a dead node.
      if (b.has_failed && now - b.failed_at < proxy_.opts_.reconnect_backoff) {
        deliver(client_id, seq, part, unavailable(b.addr), true);
        return;
      }
      if (!connect(b, now)) {
        deliver(client_id, seq, part, unavailable(b.addr), true);
        return;
      }
    }
    b.out += request;
    b.pending.push_back(Pending{client_id, seq, part, now});
    proxy_.forwarded_.fetch_add(1);
    // Flushed in settle(): many commands queued in this round go out in one
    // send() call.
    mark_dirty(b);
  }

  bool connect(Backend& b, TimePoint now) {
    net::ConnectStatus status;
    const int fd = net::start_connect(b.addr.host, b.addr.port, status);
    if (fd < 0) {
      mark_failed(b, now);
      return false;
    }
    b.fd = fd;
    fd_backend_[fd] = &b;
    poller_.add(fd);
    if (status == net::ConnectStatus::Connected) {
      b.state = Backend::State::Connected;
    } else {
      // A non-blocking connect reports completion as "writable".
      b.state = Backend::State::Connecting;
      b.connect_started = now;
      poller_.set_interest(fd, true, true);
      b.watch_write = true;
    }
    return true;
  }

  void on_backend_event(Backend& b, const PollEvent& ev) {
    if (b.state == Backend::State::Connecting) {
      if (net::socket_error(b.fd) != 0) {
        fail_backend(b);
        return;
      }
      b.state = Backend::State::Connected;
      mark_dirty(b);
    }
    if (ev.readable) {
      ssize_t n;
      do {
        n = ::recv(b.fd, read_buf_.data(), read_buf_.size(), 0);
      } while (n < 0 && errno == EINTR);
      if (n == 0 || (n < 0 && !net::would_block())) {
        fail_backend(b);
        return;
      }
      if (n > 0) {
        b.in.append(read_buf_.data(), static_cast<std::size_t>(n));
        if (!consume_replies(b)) {
          fail_backend(b);
          return;
        }
      }
    }
    if (ev.writable) mark_dirty(b);
  }

  // Matches complete replies to pending requests, in order.
  bool consume_replies(Backend& b) {
    std::size_t off = 0;
    for (;;) {
      std::size_t len = 0;
      const auto st = resp::frame_reply(std::string_view(b.in).substr(off), proxy_.opts_.max_reply_bytes, len);
      if (st == resp::ParseStatus::Incomplete) break;
      if (st == resp::ParseStatus::Error || b.pending.empty()) return false;  // node misbehaving
      const Pending p = b.pending.front();
      b.pending.pop_front();
      deliver(p.client_id, p.seq, p.part, std::string_view(b.in).substr(off, len), false);
      off += len;
    }
    b.in.erase(0, off);
    return true;
  }

  void flush_backend(Backend& b) {
    if (b.state != Backend::State::Connected) return;
    while (!b.out.empty()) {
      const ssize_t n = ::send(b.fd, b.out.data(), b.out.size(), net::kSendFlags);
      if (n > 0) {
        b.out.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && net::would_block()) break;
      fail_backend(b);
      return;
    }
    const bool write = !b.out.empty();
    if (write != b.watch_write) {
      poller_.set_interest(b.fd, true, write);
      b.watch_write = write;
    }
  }

  void mark_failed(Backend& b, TimePoint now) {
    b.has_failed = true;
    b.failed_at = now;
    proxy_.node_failures_.fetch_add(1);
  }

  // Connection to a node broke (refused, reset, timed out, garbage). Every
  // request waiting on it is answered as failed (reads then fail over); the
  // next request after the backoff period tries to reconnect.
  void fail_backend(Backend& b) {
    if (b.fd >= 0) {
      poller_.remove(b.fd);
      ::close(b.fd);
      fd_backend_.erase(b.fd);
      b.fd = -1;
    }
    b.state = Backend::State::Idle;
    b.in.clear();
    b.out.clear();
    b.watch_write = false;
    mark_failed(b, Clock::now());

    const std::string err = unavailable(b.addr);
    std::deque<Pending> pending = std::move(b.pending);
    b.pending.clear();
    for (const Pending& p : pending) deliver(p.client_id, p.seq, p.part, err, true);
  }

  void check_timeouts() {
    const TimePoint now = Clock::now();
    for (auto& [key, bp] : backends_) {
      Backend& b = *bp;
      if (b.state == Backend::State::Connecting && now - b.connect_started >= proxy_.opts_.connect_timeout) {
        fail_backend(b);
      } else if (b.state == Backend::State::Connected && !b.pending.empty() &&
                 now - b.pending.front().queued_at >= proxy_.opts_.request_timeout) {
        fail_backend(b);
      }
    }
  }

  ProxyServer& proxy_;
  Poller poller_;
  std::vector<char> read_buf_;
  resp::Limits limits_;
  int wake_r_ = -1;
  int wake_w_ = -1;

  std::unordered_map<std::uint64_t, Client> clients_;
  std::unordered_map<int, std::uint64_t> fd_client_;
  std::uint64_t next_client_id_ = 1;

  std::unordered_map<std::string, std::unique_ptr<Backend>> backends_;  // key: id@host:port
  std::unordered_map<int, Backend*> fd_backend_;

  std::vector<std::uint64_t> dirty_clients_;
  std::vector<Backend*> dirty_backends_;  // each at most once (Backend::dirty)
  std::vector<Backend*> flushing_;

  // Per-membership: the connection for each node, so forwarding is a vector
  // index instead of building an "id@host:port" string and hashing it.
  std::vector<Backend*> node_backend_;
  // Reused scratch buffers (no allocation per request once warmed up).
  std::vector<std::uint32_t> owners_buf_;
  std::vector<Backend*> targets_buf_;
  std::vector<std::uint32_t> per_key_targets_;
  std::vector<Backend*> failover_buf_;
  std::string request_buf_;

  std::shared_ptr<const Membership> m_;
  std::uint64_t m_version_ = 0;
};

// ---- ProxyServer: lifecycle and membership ------------------------------------------------

ProxyServer::ProxyServer(ProxyOptions opts) : opts_(std::move(opts)) { set_nodes(opts_.nodes); }

ProxyServer::~ProxyServer() { stop(); }

void ProxyServer::set_nodes(std::vector<NodeAddress> nodes) {
  if (nodes.empty()) throw std::invalid_argument("proxy needs at least one node");
  auto m = std::make_shared<Membership>(std::move(nodes), opts_.vnodes_per_node, opts_.replication_factor);
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(membership_mu_);
    if (membership_) {
      changed = true;
      // Nodes that were already members keep their health state; new nodes
      // hold no data yet, so they start Recovering (written to, not read from).
      for (std::size_t i = 0; i < m->nodes.size(); ++i) {
        NodeState s = NodeState::Recovering;
        for (std::size_t j = 0; j < membership_->nodes.size(); ++j) {
          if (membership_->nodes[j] == m->nodes[i]) s = membership_->state_of(j);
        }
        m->set_state(i, s);
      }
    }
    membership_ = std::move(m);
  }
  membership_version_.fetch_add(1, std::memory_order_release);
  if (changed) {
    {
      std::lock_guard<std::mutex> lock(health_mu_);
      rebalance_requested_ = true;
    }
    health_cv_.notify_all();
  }
}

std::shared_ptr<const ProxyServer::Membership> ProxyServer::membership() const {
  std::lock_guard<std::mutex> lock(membership_mu_);
  return membership_;
}

std::string ProxyServer::owner_of(std::string_view key) const {
  const auto m = membership();
  return m->nodes[m->owner(key)].id;
}

std::vector<std::string> ProxyServer::owners_of(std::string_view key) const {
  const auto m = membership();
  std::vector<std::string> out;
  for (std::size_t i : m->owners(key)) out.push_back(m->nodes[i].id);
  return out;
}

std::string ProxyServer::node_state(const std::string& node_id) const {
  const auto m = membership();
  auto it = m->index.find(node_id);
  return it == m->index.end() ? "unknown" : to_string(m->state_of(it->second));
}

std::uint64_t ProxyServer::next_version() {
  // Microseconds since 1970, shifted left 8 bits, with this proxy's id in the
  // low 8 bits: increasing over time, and unique across up to 256 proxies.
  const auto now_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  const std::uint64_t from_clock = (now_us << 8) | opts_.proxy_id;
  std::uint64_t prev = last_version_.load();
  for (;;) {
    // Never go backwards, even if the system clock is adjusted backwards.
    const std::uint64_t next = std::max(from_clock, (((prev >> 8) + 1) << 8) | opts_.proxy_id);
    if (last_version_.compare_exchange_weak(prev, next)) return next;
  }
}

void ProxyServer::start() {
  if (started_) return;
  listen_fd_ = net::create_listener(opts_.net.bind_address, opts_.net.port, opts_.net.listen_backlog,
                                    bound_port_);
  stopping_ = false;
  try {
    for (std::size_t i = 0; i < opts_.net.worker_threads; ++i) {
      workers_.push_back(std::make_unique<Worker>(*this));
    }
  } catch (...) {
    workers_.clear();
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw;
  }
  for (auto& w : workers_) threads_.emplace_back([&w] { w->run(); });
  health_thread_ = std::thread([this] { health_loop(); });
  started_ = true;
}

void ProxyServer::stop() {
  if (!started_) return;
  stopping_ = true;
  for (auto& w : workers_) w->wake();
  {
    std::lock_guard<std::mutex> lock(health_mu_);
  }
  health_cv_.notify_all();
  for (auto& t : threads_) t.join();
  health_thread_.join();
  threads_.clear();
  workers_.clear();
  ::close(listen_fd_);
  listen_fd_ = -1;
  started_ = false;
}

void ProxyServer::log_event(const std::string& msg) const {
  if (!opts_.log_events) return;
  std::printf("proxy: %s\n", msg.c_str());
  std::fflush(stdout);
}

void ProxyServer::append_info(std::string& out) const {
  const auto m = membership();
  out += "# Proxy\r\n";
  out += "connected_clients:" + std::to_string(active_.load()) + "\r\n";
  out += "total_connections_received:" + std::to_string(total_accepted_.load()) + "\r\n";
  out += "rejected_connections:" + std::to_string(rejected_.load()) + "\r\n";
  out += "total_commands_processed:" + std::to_string(commands_.load()) + "\r\n";
  out += "forwarded_requests:" + std::to_string(forwarded_.load()) + "\r\n";
  out += "node_failures:" + std::to_string(node_failures_.load()) + "\r\n";
  out += "read_failovers:" + std::to_string(failovers_.load()) + "\r\n";
  out += "protocol_errors:" + std::to_string(protocol_errors_.load()) + "\r\n";
  out += "worker_threads:" + std::to_string(opts_.net.worker_threads) + "\r\n";
  out += "used_cpu_seconds:" + std::to_string(process_cpu_seconds()) + "\r\n";
  out += "peak_rss_bytes:" + std::to_string(process_peak_rss_bytes()) + "\r\n";
  out += "# Replication\r\n";
  out += "replication_factor:" + std::to_string(opts_.replication_factor) + "\r\n";
  out += "write_quorum:" + std::to_string(opts_.write_quorum) + "\r\n";
  out += "recoveries_completed:" + std::to_string(recoveries_.load()) + "\r\n";
  out += "rebalances_completed:" + std::to_string(rebalances_.load()) + "\r\n";
  out += "# Cluster\r\n";
  out += "nodes:" + std::to_string(m->nodes.size()) + "\r\n";
  out += "vnodes_per_node:" + std::to_string(opts_.vnodes_per_node) + "\r\n";
  for (std::size_t i = 0; i < m->nodes.size(); ++i) {
    out += "node" + std::to_string(i) + ":" + m->nodes[i].to_string() + ",state=" + to_string(m->state_of(i)) +
           "\r\n";
  }
}

// ---- Health checks, recovery and rebalancing (background thread) ---------------------
//
// This thread may block (it uses NodeClient, with timeouts): it never serves
// clients, so waiting here doesn't slow anyone down.

bool ProxyServer::sleep_unless_stopping(Millis d) {
  std::unique_lock<std::mutex> lock(health_mu_);
  return !health_cv_.wait_for(lock, d, [this] { return stopping_.load(); });
}

NodeClient& ProxyServer::client_for(Clients& clients, const NodeAddress& a) {
  auto& c = clients[a.to_string()];
  if (!c) {
    c = std::make_unique<NodeClient>(a.host, a.port, opts_.health_timeout, opts_.max_reply_bytes,
                                     opts_.net.read_chunk_bytes);
  }
  return *c;
}

void ProxyServer::health_loop() {
  Clients clients;
  std::map<std::string, std::size_t> failures;  // consecutive failed checks per node
  std::map<std::string, std::string> run_ids;   // last run id seen per node
  while (!stopping_) {
    const auto m = membership();
    if (rebalance_requested_.exchange(false)) rebalance(*m, clients);

    for (std::size_t i = 0; i < m->nodes.size() && !stopping_; ++i) {
      const NodeAddress& a = m->nodes[i];
      // The health check asks for the node's run id rather than a plain PING:
      // it proves the node is alive AND tells us if it restarted.
      std::string raw;
      resp::Value v;
      const bool ok = client_for(clients, a).call({"RUNID"}, raw) && resp::decode_reply(raw, v) &&
                      v.type == resp::Value::Type::Bulk;
      auto& fails = failures[a.to_string()];
      if (ok) {
        fails = 0;
        std::string& known = run_ids[a.to_string()];
        const bool restarted = !known.empty() && known != v.str;
        known = v.str;
        if (m->state_of(i) == NodeState::Down) {
          recover(*m, i, clients);
        } else if (restarted) {
          log_event("node " + a.to_string() + " restarted (new run id)");
          recover(*m, i, clients);
        }
      } else if (m->state_of(i) != NodeState::Down && ++fails >= opts_.health_fail_threshold) {
        m->set_state(i, NodeState::Down);
        log_event("node " + a.to_string() + " is DOWN (" + std::to_string(fails) + " failed health checks)");
      }
    }

    std::unique_lock<std::mutex> lock(health_mu_);
    health_cv_.wait_for(lock, opts_.health_interval,
                        [this] { return stopping_.load() || rebalance_requested_.load(); });
  }
}

// A Down node answers again. It may hold outdated values (it missed writes
// while away), so: wipe it, let it receive new writes, wait out writes that
// were already in flight, copy in every key it should hold, then serve reads.
void ProxyServer::recover(const Membership& m, std::size_t node, Clients& clients) {
  const NodeAddress& a = m.nodes[node];
  log_event("node " + a.to_string() + " answered again: recovering");
  std::string reply;
  if (!client_for(clients, a).call({"FLUSHALL"}, reply) || reply != "+OK\r\n") {
    m.set_state(node, NodeState::Down);  // retried on a later health check
    return;
  }
  m.set_state(node, NodeState::Recovering);
  if (!sleep_unless_stopping(opts_.recovery_grace)) return;
  if (fill(m, node, clients)) {
    m.set_state(node, NodeState::Up);
    recoveries_.fetch_add(1);
    log_event("node " + a.to_string() + " is UP (recovered)");
  } else {
    m.set_state(node, NodeState::Down);  // try again on a later health check
    log_event("node " + a.to_string() + " recovery failed; will retry");
  }
}

// After a membership change: give every node the keys it now owns (fill),
// then remove the keys it no longer owns (trim). Fill-before-trim means no
// key is ever deleted from its last copy mid-way.
void ProxyServer::rebalance(const Membership& m, Clients& clients) {
  log_event("membership changed: rebalancing " + std::to_string(m.nodes.size()) + " nodes");
  if (!sleep_unless_stopping(opts_.recovery_grace)) return;
  for (std::size_t i = 0; i < m.nodes.size() && !stopping_; ++i) {
    if (m.state_of(i) == NodeState::Down) continue;  // refilled when it recovers
    if (fill(m, i, clients) && m.state_of(i) == NodeState::Recovering) m.set_state(i, NodeState::Up);
  }
  for (std::size_t i = 0; i < m.nodes.size() && !stopping_; ++i) {
    if (m.state_of(i) != NodeState::Down) trim(m, i, clients);
  }
  rebalances_.fetch_add(1);
  log_event("rebalance complete");
}

// Copies to `target` every entry it should hold, from every other reachable
// node, keeping the original versions (so nothing newer is overwritten) and
// copying tombstones as deletes (so nothing deleted comes back).
bool ProxyServer::fill(const Membership& m, std::size_t target, Clients& clients) {
  NodeClient& dst = client_for(clients, m.nodes[target]);
  std::vector<std::vector<std::string>> batch;
  std::vector<std::string> replies;
  auto flush = [&]() {
    if (batch.empty()) return true;
    const bool ok = dst.call(batch, replies);
    batch.clear();
    return ok;
  };

  for (std::size_t src = 0; src < m.nodes.size() && !stopping_; ++src) {
    if (src == target || m.state_of(src) == NodeState::Down) continue;
    NodeClient& from = client_for(clients, m.nodes[src]);
    std::size_t shards = 1;
    for (std::size_t shard = 0; shard < shards && !stopping_; ++shard) {
      std::string raw;
      resp::Value v;
      if (!from.call({"SCAN", std::to_string(shard)}, raw) || !resp::decode_reply(raw, v) ||
          v.type != resp::Value::Type::Array || v.elements.size() != 2) {
        // Source unreachable (or reply over proxy.max_reply_bytes): keep
        // going with the other copies.
        log_event("fill " + m.nodes[target].id + ": could not read shard " + std::to_string(shard) +
                  " from " + m.nodes[src].id);
        break;
      }
      shards = static_cast<std::size_t>(v.elements[0].integer);
      for (const resp::Value& e : v.elements[1].elements) {
        if (e.elements.size() != scan_entry::kFields) continue;
        const std::string& key = e.elements[scan_entry::kKey].str;
        if (!m.owns(target, key)) continue;
        // Entries written straight to a node (plain SET) have version 0;
        // copy them as the oldest possible version so any real write wins.
        const std::string& raw_version = e.elements[scan_entry::kVersion].str;
        const std::string version = raw_version == "0" ? "1" : raw_version;
        if (e.elements[scan_entry::kTombstone].integer == 1) {
          batch.push_back({"VDEL", version, key});
        } else {
          std::vector<std::string> cmd = {"VSET", version, key, e.elements[scan_entry::kValue].str};
          const std::int64_t ttl_ms = e.elements[scan_entry::kTtlMs].integer;
          if (ttl_ms > 0) {
            cmd.push_back("PX");
            cmd.push_back(std::to_string(ttl_ms));
          }
          batch.push_back(std::move(cmd));
        }
        if (batch.size() >= opts_.resync_batch && !flush()) return false;
      }
    }
  }
  return flush() && !stopping_;
}

// Removes from `node` every entry it no longer owns under the current ring
// (stale copies left behind by a membership change).
bool ProxyServer::trim(const Membership& m, std::size_t node, Clients& clients) {
  NodeClient& c = client_for(clients, m.nodes[node]);
  std::vector<std::vector<std::string>> batch;
  std::vector<std::string> replies;
  std::size_t shards = 1;
  for (std::size_t shard = 0; shard < shards && !stopping_; ++shard) {
    std::string raw;
    resp::Value v;
    if (!c.call({"SCAN", std::to_string(shard)}, raw) || !resp::decode_reply(raw, v) ||
        v.type != resp::Value::Type::Array || v.elements.size() != 2) {
      return false;
    }
    shards = static_cast<std::size_t>(v.elements[0].integer);
    for (const resp::Value& e : v.elements[1].elements) {
      if (e.elements.size() == scan_entry::kFields && !m.owns(node, e.elements[scan_entry::kKey].str)) {
        batch.push_back({"DEL", e.elements[scan_entry::kKey].str});
      }
      if (batch.size() >= opts_.resync_batch) {
        if (!c.call(batch, replies)) return false;
        batch.clear();
      }
    }
  }
  return batch.empty() || c.call(batch, replies);
}

}  // namespace cachegrid
