#include "cachegrid/ProxyServer.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "cachegrid/Config.h"
#include "cachegrid/HashRing.h"
#include "cachegrid/Poller.h"
#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

std::string upper(const std::string& s) {
  std::string u = s;
  for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

std::string arity_error(const std::string& cmd) {
  std::string name = cmd;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return "-ERR wrong number of arguments for '" + name + "' command\r\n";
}

std::string unavailable(const NodeAddress& n) {
  return "-ERR node " + n.id + " (" + n.host + ":" + std::to_string(n.port) + ") unavailable\r\n";
}

}  // namespace

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
  // Besides the usual listener fds, every worker keeps one connection per node.
  const std::size_t internal = ListenerOptions::base_internal_fds(workers) + workers * o.nodes.size();
  o.net = ListenerOptions::from_config(cfg, "proxy", internal);

  o.vnodes_per_node = cfg.get_size("proxy.vnodes_per_node");
  o.connect_timeout = Millis(cfg.get_size("proxy.connect_timeout_ms"));
  o.request_timeout = Millis(cfg.get_size("proxy.request_timeout_ms"));
  o.reconnect_backoff = Millis(cfg.get_size("proxy.reconnect_backoff_ms"));
  o.timer_tick = Millis(cfg.get_size("proxy.timer_tick_ms"));
  o.max_pending_per_client = cfg.get_size("proxy.max_pending_per_client");
  o.max_reply_bytes = cfg.get_bytes("proxy.max_reply_bytes");

  const std::pair<std::size_t, const char*> positives[] = {
      {o.vnodes_per_node, "proxy.vnodes_per_node"},
      {static_cast<std::size_t>(o.connect_timeout.count()), "proxy.connect_timeout_ms"},
      {static_cast<std::size_t>(o.request_timeout.count()), "proxy.request_timeout_ms"},
      {static_cast<std::size_t>(o.timer_tick.count()), "proxy.timer_tick_ms"},
      {o.max_pending_per_client, "proxy.max_pending_per_client"},
      {o.max_reply_bytes, "proxy.max_reply_bytes"},
  };
  for (const auto& [v, key] : positives) {
    if (v == 0) throw ConfigError(std::string("'") + key + "' must be >= 1");
  }
  if (o.timer_tick.count() > std::numeric_limits<int>::max()) {
    throw ConfigError("'proxy.timer_tick_ms' is too large");
  }
  return o;
}

// ---- Membership ------------------------------------------------------------------

struct ProxyServer::Membership {
  Membership(std::vector<NodeAddress> n, std::size_t vnodes) : nodes(std::move(n)), ring(vnodes) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      ring.add_node(nodes[i].id);
      index.emplace(nodes[i].id, i);
    }
  }
  std::size_t owner(std::string_view key) const { return index.at(ring.owner(key)); }

  std::vector<NodeAddress> nodes;
  HashRing ring;
  std::unordered_map<std::string, std::size_t> index;  // node id -> position in `nodes`
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
    bool done = false;
    std::string reply;
    // Fan-out commands (multi-node DEL, DBSIZE): integers summed over parts.
    bool is_sum = false;
    std::size_t remaining = 0;
    std::int64_t sum = 0;
    std::string error;  // first error reply from any part
  };

  struct Client {
    int fd = -1;
    std::string in;
    std::string out;
    std::deque<Slot> slots;     // oldest first
    std::uint64_t first_seq = 0;  // sequence number of slots.front()
    bool closing = false;
    bool watch_read = true;
    bool watch_write = false;
  };

  // A command sent to a node, waiting for its reply.
  struct Pending {
    std::uint64_t client_id;
    std::uint64_t seq;
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
  // All work funnels through here, so no function ever calls back into
  // another client's processing (no re-entrancy surprises).
  void settle() {
    while (!dirty_clients_.empty() || !dirty_backends_.empty()) {
      while (!dirty_clients_.empty()) {
        const std::uint64_t id = dirty_clients_.back();
        dirty_clients_.pop_back();
        settle_client(id);
      }
      auto backends = std::move(dirty_backends_);
      dirty_backends_.clear();
      for (Backend* b : backends) flush_backend(*b);
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

    process(id, c);
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
    pump(c);
  }

  void dispatch(std::uint64_t id, Client& c, const std::vector<std::string>& args) {
    refresh_membership();
    const std::string cmd = upper(args[0]);
    const std::size_t argc = args.size();

    if (cmd == "GET" || cmd == "SET") {
      if (argc < 2) {
        push_done(c, arity_error(cmd));
        return;
      }
      const std::uint64_t seq = push_slot(c, Slot{});
      forward(m_->owner(args[1]), resp::encode_command(args), id, seq);
    } else if (cmd == "DEL") {
      if (argc < 2) {
        push_done(c, arity_error(cmd));
        return;
      }
      // Group keys by owning node: one DEL per node, sent in parallel.
      std::map<std::size_t, std::vector<std::string>> by_node;
      for (std::size_t i = 1; i < argc; ++i) {
        auto& group = by_node[m_->owner(args[i])];
        if (group.empty()) group.push_back("DEL");
        group.push_back(args[i]);
      }
      const std::uint64_t seq = push_slot(c, sum_slot(by_node.size()));
      for (const auto& [node, cmd_args] : by_node) forward(node, resp::encode_command(cmd_args), id, seq);
    } else if (cmd == "DBSIZE") {
      const std::uint64_t seq = push_slot(c, sum_slot(m_->nodes.size()));
      const std::string req = resp::encode_command({"DBSIZE"});
      for (std::size_t n = 0; n < m_->nodes.size(); ++n) forward(n, req, id, seq);
    } else if (cmd == "PING") {
      if (argc == 1) {
        push_done(c, "+PONG\r\n");
      } else if (argc == 2) {
        std::string r;
        resp::write_bulk(r, args[1]);
        push_done(c, r);
      } else {
        push_done(c, arity_error(cmd));
      }
    } else if (cmd == "INFO") {
      std::string text;
      proxy_.append_info(text);
      std::string r;
      resp::write_bulk(r, text);
      push_done(c, r);
    } else if (cmd == "QUIT") {
      push_done(c, "+OK\r\n");
      c.closing = true;
    } else if (cmd == "COMMAND") {
      push_done(c, "*0\r\n");
    } else {
      push_done(c, "-ERR unknown command '" + args[0] + "'\r\n");
    }
  }

  static Slot sum_slot(std::size_t parts) {
    Slot s;
    s.is_sum = true;
    s.remaining = parts;
    return s;
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

  // A node's reply (or an error standing in for one) for slot `seq`.
  void deliver(std::uint64_t client_id, std::uint64_t seq, std::string_view reply) {
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;  // client left; drop the reply
    Client& c = it->second;
    if (seq < c.first_seq || seq - c.first_seq >= c.slots.size()) return;
    Slot& s = c.slots[static_cast<std::size_t>(seq - c.first_seq)];

    if (s.is_sum) {
      if (!reply.empty() && reply[0] == ':') {
        s.sum += std::strtoll(std::string(reply.substr(1)).c_str(), nullptr, 10);
      } else if (s.error.empty()) {
        s.error.assign(reply);
      }
      if (--s.remaining == 0) {
        s.done = true;
        s.reply = s.error.empty() ? ":" + std::to_string(s.sum) + "\r\n" : s.error;
      }
    } else {
      s.reply.assign(reply);
      s.done = true;
    }
    pump(c);
    mark_dirty(client_id);
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

  void forward(std::size_t node_index, std::string request, std::uint64_t client_id, std::uint64_t seq) {
    Backend& b = backend_for(m_->nodes[node_index]);
    const TimePoint now = Clock::now();

    if (b.state == Backend::State::Idle) {
      // Recently failed: answer immediately instead of making every request
      // wait out a connect timeout against a dead node.
      if (b.has_failed && now - b.failed_at < proxy_.opts_.reconnect_backoff) {
        deliver(client_id, seq, unavailable(b.addr));
        return;
      }
      if (!connect(b, now)) {
        deliver(client_id, seq, unavailable(b.addr));
        return;
      }
    }
    b.out += request;
    b.pending.push_back(Pending{client_id, seq, now});
    proxy_.forwarded_.fetch_add(1);
    // Flushed in settle(): many commands queued in this round go out in one
    // send() call.
    dirty_backends_.insert(&b);
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
      dirty_backends_.insert(&b);
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
    if (ev.writable) dirty_backends_.insert(&b);
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
      deliver(p.client_id, p.seq, std::string_view(b.in).substr(off, len));
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
  // request waiting on it gets an error reply; the next request after the
  // backoff period tries to reconnect.
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
    dirty_backends_.erase(&b);
    mark_failed(b, Clock::now());

    const std::string err = unavailable(b.addr);
    std::deque<Pending> pending = std::move(b.pending);
    b.pending.clear();
    for (const Pending& p : pending) deliver(p.client_id, p.seq, err);
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
  std::unordered_set<Backend*> dirty_backends_;

  std::shared_ptr<const Membership> m_;
  std::uint64_t m_version_ = 0;
};

// ---- ProxyServer -----------------------------------------------------------------------

ProxyServer::ProxyServer(ProxyOptions opts) : opts_(std::move(opts)) { set_nodes(opts_.nodes); }

ProxyServer::~ProxyServer() { stop(); }

void ProxyServer::set_nodes(std::vector<NodeAddress> nodes) {
  if (nodes.empty()) throw std::invalid_argument("proxy needs at least one node");
  auto m = std::make_shared<const Membership>(std::move(nodes), opts_.vnodes_per_node);
  {
    std::lock_guard<std::mutex> lock(membership_mu_);
    membership_ = std::move(m);
  }
  membership_version_.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<const ProxyServer::Membership> ProxyServer::membership() const {
  std::lock_guard<std::mutex> lock(membership_mu_);
  return membership_;
}

std::string ProxyServer::owner_of(std::string_view key) const {
  const auto m = membership();
  return m->nodes[m->owner(key)].id;
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
  started_ = true;
}

void ProxyServer::stop() {
  if (!started_) return;
  stopping_ = true;
  for (auto& w : workers_) w->wake();
  for (auto& t : threads_) t.join();
  threads_.clear();
  workers_.clear();
  ::close(listen_fd_);
  listen_fd_ = -1;
  started_ = false;
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
  out += "protocol_errors:" + std::to_string(protocol_errors_.load()) + "\r\n";
  out += "worker_threads:" + std::to_string(opts_.net.worker_threads) + "\r\n";
  out += "# Cluster\r\n";
  out += "nodes:" + std::to_string(m->nodes.size()) + "\r\n";
  out += "vnodes_per_node:" + std::to_string(opts_.vnodes_per_node) + "\r\n";
  for (std::size_t i = 0; i < m->nodes.size(); ++i) {
    out += "node" + std::to_string(i) + ":" + m->nodes[i].to_string() + "\r\n";
  }
}

}  // namespace cachegrid
