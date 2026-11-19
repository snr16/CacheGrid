#include "cachegrid/CacheServer.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "cachegrid/CommandHandler.h"
#include "cachegrid/Config.h"
#include "cachegrid/Net.h"
#include "cachegrid/Poller.h"
#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

// ---- ServerOptions -----------------------------------------------------------

ServerOptions ServerOptions::from_config(const Config& cfg) {
  ServerOptions o{};
  const std::size_t workers = cfg.get_size("server.worker_threads");
  o.net = ListenerOptions::from_config(cfg, "server", ListenerOptions::base_internal_fds(workers));
  o.purge_interval = Millis(cfg.get_size("server.purge_interval_ms"));
  if (o.purge_interval.count() == 0) throw ConfigError("'server.purge_interval_ms' must be >= 1");
  return o;
}

// ---- Worker: one event-loop thread ---------------------------------------------

class CacheServer::Worker {
 public:
  explicit Worker(CacheServer& server)
      : server_(server),
        handler_(server.cache_, [&server](std::string& out) { server.append_info(out); }),
        read_buf_(server.opts_.net.read_chunk_bytes),
        limits_{server.opts_.net.max_request_bytes, server.opts_.net.max_args} {
    int fds[2];
    if (::pipe(fds) < 0) throw_errno("pipe");
    wake_r_ = fds[0];
    wake_w_ = fds[1];
    net::set_nonblocking(wake_r_);
    net::set_nonblocking(wake_w_);
    poller_.add_readonly(wake_r_, false);
    poller_.add_readonly(server.listen_fd_, true);
  }

  ~Worker() {
    for (auto& [fd, conn] : conns_) ::close(fd);
    ::close(wake_r_);
    ::close(wake_w_);
  }

  // Called from another thread to interrupt wait() during shutdown.
  void wake() {
    const char b = 1;
    [[maybe_unused]] auto n = ::write(wake_w_, &b, 1);
  }

  void run() {
    std::vector<PollEvent> events;
    const int max_events = static_cast<int>(server_.opts_.net.events_per_poll);
    while (!server_.stopping_.load()) {
      const int n = poller_.wait(events, max_events, -1);
      for (int i = 0; i < n; ++i) handle(events[static_cast<std::size_t>(i)]);
    }
    for (auto& [fd, conn] : conns_) {
      poller_.remove(fd);
      ::close(fd);
      server_.active_.fetch_sub(1);
    }
    conns_.clear();
  }

 private:
  struct Connection {
    std::string in;          // bytes received but not yet parsed
    std::string out;         // reply bytes not yet sent
    bool closing = false;    // close once `out` is flushed
    bool watch_read = true;  // what the poller is currently told to report
    bool watch_write = false;
  };

  void handle(const PollEvent& ev) {
    if (ev.fd == wake_r_) {
      // Empty the pipe (reusing the configured read buffer).
      while (::read(wake_r_, read_buf_.data(), read_buf_.size()) > 0) {
      }
      return;
    }
    if (ev.fd == server_.listen_fd_) {
      accept_all();
      return;
    }

    auto it = conns_.find(ev.fd);
    if (it == conns_.end()) return;  // closed earlier in this batch
    Connection& c = it->second;

    if (ev.readable && !c.closing && !on_readable(ev.fd, c)) {
      close_conn(ev.fd);
      return;
    }
    if (!flush(ev.fd, c)) {
      close_conn(ev.fd);
      return;
    }
    if (c.closing && c.out.empty()) {
      close_conn(ev.fd);
      return;
    }
    update_interest(ev.fd, c);
  }

  // Tell the poller what we now care about, only if it changed (each change
  // is a system call).
  void update_interest(int fd, Connection& c) {
    const bool read = !c.closing;
    const bool write = !c.out.empty();
    if (read != c.watch_read || write != c.watch_write) {
      poller_.set_interest(fd, read, write);
      c.watch_read = read;
      c.watch_write = write;
    }
  }

  void accept_all() {
    for (;;) {
      const int fd = ::accept(server_.listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (!net::would_block()) std::perror("accept");
        return;  // EAGAIN: another worker took it, or the queue is empty
      }
      server_.total_accepted_.fetch_add(1);

      if (!net::set_nonblocking(fd)) {
        ::close(fd);
        continue;
      }
      net::configure_stream_socket(fd);

      if (server_.active_.fetch_add(1) >= server_.opts_.net.max_connections) {
        server_.active_.fetch_sub(1);
        server_.rejected_.fetch_add(1);
        static constexpr std::string_view kMsg = "-ERR max number of clients reached\r\n";
        [[maybe_unused]] auto n = ::send(fd, kMsg.data(), kMsg.size(), net::kSendFlags);
        ::close(fd);
        continue;
      }
      poller_.add(fd);
      conns_.emplace(fd, Connection{});
    }
  }

  // One recv() per readiness event. With level-triggered polling we'll be
  // told again if more is waiting, and other clients get a turn in between.
  // Returns false if the connection must be closed immediately.
  bool on_readable(int fd, Connection& c) {
    ssize_t n;
    do {
      n = ::recv(fd, read_buf_.data(), read_buf_.size(), 0);
    } while (n < 0 && errno == EINTR);

    if (n == 0) {
      // Peer finished sending. Run what we already have and send the
      // replies, then close (supports `printf 'PING\r\n' | nc ...`).
      c.closing = true;
      return true;
    }
    if (n < 0) return net::would_block();

    c.in.append(read_buf_.data(), static_cast<std::size_t>(n));
    return process(c);
  }

  // Parses and executes every complete command in the input buffer
  // (pipelining: a client may send many commands before reading replies).
  bool process(Connection& c) {
    std::size_t off = 0;
    while (!c.closing) {
      resp::ParseResult r =
          resp::parse_command(std::string_view(c.in).substr(off), limits_);
      if (r.status == resp::ParseStatus::Incomplete) break;
      if (r.status == resp::ParseStatus::Error) {
        server_.protocol_errors_.fetch_add(1);
        resp::write_error(c.out, "ERR Protocol error: " + r.error);
        c.closing = true;
        break;
      }
      off += r.consumed;
      if (r.args.empty()) continue;

      server_.commands_.fetch_add(1);
      if (handler_.execute(r.args, c.out) == CommandHandler::Action::Close) c.closing = true;

      // A client that sends but never reads would make `out` grow forever.
      if (c.out.size() > server_.opts_.net.max_output_buffer) return false;
    }
    c.in.erase(0, off);
    return true;
  }

  // Sends as much pending output as the socket accepts right now. Whatever is
  // left (kernel send buffer full) stays in `out`; update_interest() then asks
  // the poller to report when there's room again.
  bool flush(int fd, Connection& c) {
    while (!c.out.empty()) {
      const ssize_t n = ::send(fd, c.out.data(), c.out.size(), net::kSendFlags);
      if (n > 0) {
        c.out.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && net::would_block()) return true;
      return false;  // peer gone (EPIPE, ECONNRESET, ...)
    }
    return true;
  }

  void close_conn(int fd) {
    poller_.remove(fd);
    ::close(fd);
    conns_.erase(fd);
    server_.active_.fetch_sub(1);
  }

  CacheServer& server_;
  CommandHandler handler_;
  Poller poller_;
  std::vector<char> read_buf_;
  resp::Limits limits_;
  std::unordered_map<int, Connection> conns_;
  int wake_r_ = -1;
  int wake_w_ = -1;
};

// ---- CacheServer ---------------------------------------------------------------

CacheServer::CacheServer(ServerOptions opts, Cache& cache) : opts_(std::move(opts)), cache_(cache) {}

CacheServer::~CacheServer() { stop(); }

void CacheServer::start() {
  if (started_) return;

  listen_fd_ = net::create_listener(opts_.net.bind_address, opts_.net.port,
                                   opts_.net.listen_backlog, bound_port_);

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
  purge_thread_ = std::thread([this] { purge_loop(); });
  started_ = true;
}

void CacheServer::stop() {
  if (!started_) return;
  stopping_ = true;
  for (auto& w : workers_) w->wake();
  {
    // Taking the lock (even empty) guarantees the purge thread is either
    // waiting or about to re-check `stopping_`, so the notify can't be missed.
    std::lock_guard<std::mutex> lock(purge_mu_);
  }
  purge_cv_.notify_all();

  for (auto& t : threads_) t.join();
  purge_thread_.join();
  threads_.clear();
  workers_.clear();
  ::close(listen_fd_);
  listen_fd_ = -1;
  started_ = false;
}

void CacheServer::purge_loop() {
  std::unique_lock<std::mutex> lock(purge_mu_);
  while (!purge_cv_.wait_for(lock, opts_.purge_interval, [this] { return stopping_.load(); })) {
    lock.unlock();
    cache_.purge_expired();
    lock.lock();
  }
}

void CacheServer::append_info(std::string& out) const {
  out += "connected_clients:" + std::to_string(active_.load()) + "\r\n";
  out += "total_connections_received:" + std::to_string(total_accepted_.load()) + "\r\n";
  out += "rejected_connections:" + std::to_string(rejected_.load()) + "\r\n";
  out += "total_commands_processed:" + std::to_string(commands_.load()) + "\r\n";
  out += "protocol_errors:" + std::to_string(protocol_errors_.load()) + "\r\n";
  out += "worker_threads:" + std::to_string(opts_.net.worker_threads) + "\r\n";
}

}  // namespace cachegrid
