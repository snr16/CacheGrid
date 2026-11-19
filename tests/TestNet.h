#pragma once

// Networking helpers for end-to-end tests: a minimal blocking RESP client and
// an in-process cache node on a random free port.

#include "cachegrid/CacheServer.h"
#include "TestUtil.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cachegrid::test {

// ---- Test fixture values (independent of the shipped config) -------------------


inline ServerOptions test_server_opts() {
  ServerOptions o{};
  o.net = test_listener_opts();
  o.purge_interval = cfg_ms("test.server.purge_interval_ms");
  return o;
}

inline std::string encode(const std::vector<std::string>& args) {
  std::string s = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return s;
}

// End index of the complete RESP reply starting at `pos`, or npos if more
// bytes are needed.
inline std::size_t reply_end(std::string_view b, std::size_t pos) {
  if (pos >= b.size()) return std::string_view::npos;
  const std::size_t crlf = b.find("\r\n", pos);
  if (crlf == std::string_view::npos) return std::string_view::npos;
  const char type = b[pos];
  if (type == '+' || type == '-' || type == ':') return crlf + 2;
  const long long n = std::stoll(std::string(b.substr(pos + 1, crlf - pos - 1)));
  if (type == '$') {
    if (n < 0) return crlf + 2;
    const std::size_t end = crlf + 2 + static_cast<std::size_t>(n) + 2;
    return end <= b.size() ? end : std::string_view::npos;
  }
  if (type == '*') {
    std::size_t p = crlf + 2;
    for (long long i = 0; i < n; ++i) {
      p = reply_end(b, p);
      if (p == std::string_view::npos) return p;
    }
    return p;
  }
  throw std::runtime_error("bad reply type");
}

// Minimal blocking client.
class Client {
 public:
  explicit Client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      throw std::runtime_error("connect failed");
    }
    // A hung test fails after this instead of blocking forever.
    const auto timeout = cfg_ms("test.client.recv_timeout_ms");
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  }
  ~Client() { close(); }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  void send_raw(std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = ::send(fd_, data.data(), data.size(), net::kSendFlags);
      if (n <= 0) throw std::runtime_error("send failed");
      data.remove_prefix(static_cast<std::size_t>(n));
    }
  }

  void shutdown_write() { ::shutdown(fd_, SHUT_WR); }

  // Reads exactly one complete reply. Returns "" if the server closed first.
  std::string read_reply() {
    for (;;) {
      const std::size_t end = reply_end(buf_, 0);
      if (end != std::string::npos) {
        std::string r = buf_.substr(0, end);
        buf_.erase(0, end);
        return r;
      }
      char tmp[64 * 1024];
      const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
      if (n <= 0) return "";
      buf_.append(tmp, static_cast<std::size_t>(n));
    }
  }

  std::string command(const std::vector<std::string>& args) {
    send_raw(encode(args));
    return read_reply();
  }

  // True once the server has closed the connection (EOF or reset).
  bool wait_closed() {
    char tmp[64 * 1024];
    for (;;) {
      const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
      if (n == 0) return true;
      if (n < 0) return errno != EAGAIN && errno != EWOULDBLOCK;  // timeout = still open
    }
  }

 private:
  int fd_ = -1;
  std::string buf_;
};

struct TestServer {
  explicit TestServer(ServerOptions opts = test_server_opts())
      : cache(cache_opts(cfg_bytes("test.server.cache_capacity"), cfg_size("test.server.cache_shards"))),
        server(std::move(opts), cache) {
    server.start();
  }
  std::uint16_t port() const { return server.port(); }

  Cache cache;
  CacheServer server;
};

// Polls `cond` until true or a deadline passes.
template <typename Fn>
bool eventually(Fn cond) {
  const auto deadline = std::chrono::steady_clock::now() + cfg_ms("test.client.recv_timeout_ms");
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(cfg_ms("test.client.poll_interval_ms"));
  }
  return cond();
}

}  // namespace cachegrid::test
