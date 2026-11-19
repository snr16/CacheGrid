#pragma once

// Socket helpers and listener settings shared by the cache node and the proxy.

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace cachegrid {

class Config;

// Settings for any component that accepts client connections. Read from
// `<prefix>.*` keys, e.g. "server.port" for a node, "proxy.port" for the proxy.
struct ListenerOptions {
  std::string bind_address;       // IPv4 address to listen on
  std::uint16_t port;             // 0 = let the OS pick a free port (tests)
  std::size_t worker_threads;     // event-loop threads (the bounded pool)
  std::size_t max_connections;    // beyond this, new clients get an error
  int listen_backlog;             // kernel queue of not-yet-accepted connections
  std::size_t max_request_bytes;  // largest single command
  std::size_t max_args;           // most arguments in one command
  std::size_t max_output_buffer;  // per client; exceeded = client disconnected
  std::size_t read_chunk_bytes;   // bytes read per recv() call
  std::size_t events_per_poll;    // most events handled per wake-up
  std::size_t fd_reserve;         // file descriptors kept for internal use

  // `internal_fds` = descriptors the component needs besides client sockets;
  // fd_reserve must cover it.
  static ListenerOptions from_config(const Config& cfg, const std::string& prefix,
                                     std::size_t internal_fds);

  // Descriptors every listener needs: per worker a poller and a wake-up pipe
  // pair, plus the listening socket, stdin/out/err, and one fd briefly used
  // when rejecting a client over the limit.
  static std::size_t base_internal_fds(std::size_t workers) { return 3 * workers + 5; }
};

namespace net {

// Linux: don't raise SIGPIPE when sending to a closed socket (macOS uses the
// SO_NOSIGPIPE socket option instead, set in configure_stream_socket).
#ifdef MSG_NOSIGNAL
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

bool set_nonblocking(int fd);

// TCP_NODELAY (send small messages immediately, no Nagle delay) and, on
// macOS, SO_NOSIGPIPE.
void configure_stream_socket(int fd);

// True if the last socket call failed only because it would have blocked.
bool would_block();

// Creates a non-blocking listening socket. Returns the fd and stores the
// actual port in `bound_port`. Throws std::system_error on failure.
int create_listener(const std::string& bind_address, std::uint16_t port, int backlog,
                    std::uint16_t& bound_port);

enum class ConnectStatus { Connected, InProgress, Failed };

// Starts a non-blocking TCP connect to host:port (host may be a name, e.g. a
// Docker service name). Returns the fd, or -1 with status Failed.
// Note: name resolution itself (getaddrinfo) is blocking.
int start_connect(const std::string& host, std::uint16_t port, ConnectStatus& status);

// Result of a finished non-blocking connect: 0 on success, else an errno.
int socket_error(int fd);

// Every connection is a file descriptor, and the OS caps how many a process
// may open ("ulimit -n"). Raises this process's soft limit to `needed`, or
// throws std::runtime_error if that isn't allowed: hitting the cap at runtime
// would make accept() fail in a tight loop.
void ensure_fd_limit(std::size_t needed);

}  // namespace net
}  // namespace cachegrid
