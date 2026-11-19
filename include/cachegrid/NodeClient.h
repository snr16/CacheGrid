#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cachegrid/LRUCache.h"  // Millis

namespace cachegrid {

// A simple *blocking* RESP client for one node, with a timeout on every
// operation. Used by the proxy's background health/recovery thread, where
// waiting is fine (it isn't serving clients) but hanging forever is not.
//
// Unlike the proxy's event-loop workers, this sends a batch of commands and
// then waits for exactly that many replies.
class NodeClient {
 public:
  NodeClient(std::string host, std::uint16_t port, Millis timeout, std::size_t max_reply_bytes,
             std::size_t read_chunk_bytes);
  ~NodeClient();
  NodeClient(const NodeClient&) = delete;
  NodeClient& operator=(const NodeClient&) = delete;

  // Sends every command (pipelined), then reads one reply per command into
  // `replies` (raw RESP bytes). Connects first if needed. On any failure
  // (refused, timeout, malformed reply) closes the connection and returns
  // false; the next call reconnects.
  bool call(const std::vector<std::vector<std::string>>& commands, std::vector<std::string>& replies);

  // Convenience for a single command.
  bool call(const std::vector<std::string>& command, std::string& reply);

  void close();
  const std::string& host() const { return host_; }
  std::uint16_t port() const { return port_; }

 private:
  bool connect();

  std::string host_;
  std::uint16_t port_;
  Millis timeout_;
  std::size_t max_reply_bytes_;
  int fd_ = -1;
  std::string in_;
  std::vector<char> read_buf_;
};

}  // namespace cachegrid
