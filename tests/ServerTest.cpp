// End-to-end tests: a real server on a random free port, real TCP sockets.

#include "cachegrid/CacheServer.h"
#include "TestUtil.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace cachegrid;
using namespace cachegrid::test;

namespace {

// ---- Test fixture values (independent of the shipped config) -------------------

constexpr int kRecvTimeoutSec = 5;  // a hung test fails instead of blocking forever

ServerOptions test_server_opts() {
  ServerOptions o{};
  o.bind_address = "127.0.0.1";
  o.port = 0;  // OS picks a free port, so tests never collide
  o.worker_threads = 2;
  o.max_connections = 64;
  o.listen_backlog = 128;
  o.max_request_bytes = 1 << 20;
  o.max_args = 1024;
  o.max_output_buffer = 16 << 20;
  o.read_chunk_bytes = 16 << 10;
  o.events_per_poll = 64;
  o.purge_interval = Millis(50);
  o.fd_reserve = 64;
  return o;
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::string encode(const std::vector<std::string>& args) {
  std::string s = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return s;
}

// End index of the complete RESP reply starting at `pos`, or npos if more
// bytes are needed.
std::size_t reply_end(std::string_view b, std::size_t pos) {
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
    timeval tv{kRecvTimeoutSec, 0};
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
      const ssize_t n = ::send(fd_, data.data(), data.size(), kSendFlags);
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
      : cache(cache_opts(64 << 20, 8)), server(std::move(opts), cache) {
    server.start();
  }
  std::uint16_t port() const { return server.port(); }

  Cache cache;
  CacheServer server;
};

// Polls `cond` until true or a deadline passes.
template <typename Fn>
bool eventually(Fn cond) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRecvTimeoutSec);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

}  // namespace

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
  std::string batch;
  for (int i = 0; i < 500; ++i) batch += encode({"SET", "k" + std::to_string(i), std::to_string(i)});
  for (int i = 0; i < 500; ++i) batch += encode({"GET", "k" + std::to_string(i)});
  c.send_raw(batch);
  for (int i = 0; i < 500; ++i) ASSERT_EQ(c.read_reply(), "+OK\r\n");
  for (int i = 0; i < 500; ++i) {
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
  o.max_request_bytes = 1024;
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
  o.max_output_buffer = 64 * 1024;
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
  o.max_connections = 2;
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
  constexpr int kClients = 32;
  constexpr int kOps = 200;
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
