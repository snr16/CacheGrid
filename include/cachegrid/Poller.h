#pragma once

// Thin wrapper over the OS readiness-notification API:
//   - kqueue on macOS / BSD
//   - epoll  on Linux
//
// Both answer the same question: "of all these sockets, which ones can I read
// from or write to right now without blocking?" One thread can then serve
// thousands of connections instead of needing one thread per connection.
//
// Level-triggered: as long as a socket has unread data (or room to write),
// every wait() reports it again. That's simpler to get right than
// edge-triggered mode, where a missed event can stall a connection forever.

#include <vector>

namespace cachegrid {

struct PollEvent {
  int fd = -1;
  bool readable = false;  // also set on hang-up/error, so the read path notices
  bool writable = false;
};

class Poller {
 public:
  Poller();
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  // Client connection: starts watched for reads only.
  void add(int fd);

  // Read-only fd such as the listening socket or a wake-up pipe. `shared`
  // marks a listening socket watched by several pollers (on Linux this uses
  // EPOLLEXCLUSIVE so one accept wakes one thread, not all of them).
  void add_readonly(int fd, bool shared);

  // Choose which events to be told about for a client connection.
  //   read:  off once we've decided to close (otherwise a half-closed socket
  //          reports "readable" forever and the loop spins).
  //   write: on only while a reply is waiting to be sent; a socket with room
  //          in its send buffer is almost always writable, so leaving it on
  //          would wake us constantly.
  void set_interest(int fd, bool read, bool write);

  // Stop watching fd. Call before close().
  void remove(int fd);

  // Blocks until at least one event or timeout_ms (-1 = forever). Fills `out`
  // (up to max_events) and returns the count. Returns 0 if interrupted.
  int wait(std::vector<PollEvent>& out, int max_events, int timeout_ms);

 private:
  int fd_;
};

}  // namespace cachegrid
