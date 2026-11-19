#include "cachegrid/Net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "cachegrid/Config.h"

namespace cachegrid {

ListenerOptions ListenerOptions::from_config(const Config& cfg, const std::string& prefix,
                                             std::size_t internal_fds) {
  auto key = [&](const char* name) { return prefix + "." + name; };
  auto require_positive = [&](std::size_t v, const char* name) {
    if (v == 0) throw ConfigError("'" + key(name) + "' must be >= 1");
  };

  ListenerOptions o{};
  o.bind_address = cfg.get_string(key("bind_address"));
  const std::size_t port = cfg.get_size(key("port"));
  if (port > 65535) throw ConfigError("'" + key("port") + "' must be 0..65535");
  o.port = static_cast<std::uint16_t>(port);
  o.worker_threads = cfg.get_size(key("worker_threads"));
  o.max_connections = cfg.get_size(key("max_connections"));
  const std::size_t backlog = cfg.get_size(key("listen_backlog"));
  o.max_request_bytes = cfg.get_bytes(key("max_request_bytes"));
  o.max_args = cfg.get_size(key("max_args"));
  o.max_output_buffer = cfg.get_bytes(key("max_output_buffer"));
  o.read_chunk_bytes = cfg.get_bytes(key("read_chunk_bytes"));
  o.events_per_poll = cfg.get_size(key("events_per_poll"));
  o.fd_reserve = cfg.get_size(key("fd_reserve"));

  require_positive(o.worker_threads, "worker_threads");
  require_positive(o.max_connections, "max_connections");
  require_positive(backlog, "listen_backlog");
  require_positive(o.max_request_bytes, "max_request_bytes");
  require_positive(o.max_args, "max_args");
  require_positive(o.max_output_buffer, "max_output_buffer");
  require_positive(o.read_chunk_bytes, "read_chunk_bytes");
  require_positive(o.events_per_poll, "events_per_poll");
  if (backlog > 65535) throw ConfigError("'" + key("listen_backlog") + "' must be <= 65535");
  if (o.events_per_poll > 65535) throw ConfigError("'" + key("events_per_poll") + "' must be <= 65535");
  o.listen_backlog = static_cast<int>(backlog);

  if (o.fd_reserve < internal_fds) {
    throw ConfigError("'" + key("fd_reserve") + "' must be >= " + std::to_string(internal_fds) +
                      " for this configuration");
  }
  return o;
}

namespace net {

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void configure_stream_socket(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

int create_listener(const std::string& bind_address, std::uint16_t port, int backlog,
                    std::uint16_t& bound_port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    throw std::invalid_argument("invalid bind address '" + bind_address + "'");
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");

  // Allow restarting immediately on the same port (otherwise the OS holds it
  // in TIME_WAIT for up to a minute after the previous run exits).
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  auto fail = [fd](const std::string& what) {
    const int err = errno;
    ::close(fd);
    throw std::system_error(err, std::generic_category(), what);
  };
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    fail("bind " + bind_address + ":" + std::to_string(port));
  }
  if (::listen(fd, backlog) < 0) fail("listen");
  if (!set_nonblocking(fd)) fail("fcntl");

  sockaddr_in bound{};
  socklen_t len = sizeof bound;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
  bound_port = ntohs(bound.sin_port);
  return fd;
}

int start_connect(const std::string& host, std::uint16_t port, ConnectStatus& status) {
  status = ConnectStatus::Failed;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    return -1;
  }

  const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0 || !set_nonblocking(fd)) {
    if (fd >= 0) ::close(fd);
    ::freeaddrinfo(res);
    return -1;
  }
  configure_stream_socket(fd);

  const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
  ::freeaddrinfo(res);
  if (rc == 0) {
    status = ConnectStatus::Connected;
  } else if (errno == EINPROGRESS) {
    status = ConnectStatus::InProgress;
  } else {
    ::close(fd);
    return -1;
  }
  return fd;
}

int socket_error(int fd) {
  int err = 0;
  socklen_t len = sizeof err;
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
  return err;
}

void ensure_fd_limit(std::size_t needed) {
  rlimit rl{};
  if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
  if (rl.rlim_cur >= needed) return;

  rlimit want = rl;
  want.rlim_cur = static_cast<rlim_t>(needed);
  const bool hard_ok = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= needed;
  if (!hard_ok || ::setrlimit(RLIMIT_NOFILE, &want) != 0) {
    throw std::runtime_error("max_connections + fd_reserve = " + std::to_string(needed) +
                             " exceeds the open-file limit (" + std::to_string(rl.rlim_cur) +
                             "); lower max_connections or raise `ulimit -n`");
  }
}

}  // namespace net
}  // namespace cachegrid
