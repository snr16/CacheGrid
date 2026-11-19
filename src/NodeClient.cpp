#include "cachegrid/NodeClient.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

#include "cachegrid/Net.h"
#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

using SteadyClock = std::chrono::steady_clock;

// Waits until `fd` is ready for `events` (POLLIN/POLLOUT) or the deadline
// passes. Returns false on timeout or error.
bool wait_ready(int fd, short events, SteadyClock::time_point deadline) {
  for (;;) {
    const auto now = SteadyClock::now();
    if (now >= deadline) return false;
    // Round UP: rounding down would give up to 1ms before the deadline.
    const auto left = std::chrono::ceil<Millis>(deadline - now).count();
    pollfd p{fd, events, 0};
    const int n = ::poll(&p, 1, static_cast<int>(left));
    if (n > 0) return (p.revents & (events | POLLHUP | POLLERR)) != 0;
    if (n == 0) return false;
    if (errno != EINTR) return false;
  }
}

}  // namespace

NodeClient::NodeClient(std::string host, std::uint16_t port, Millis timeout, std::size_t max_reply_bytes,
                       std::size_t read_chunk_bytes)
    : host_(std::move(host)),
      port_(port),
      timeout_(timeout),
      max_reply_bytes_(max_reply_bytes),
      read_buf_(read_chunk_bytes) {}

NodeClient::~NodeClient() { close(); }

void NodeClient::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  in_.clear();
}

bool NodeClient::connect() {
  net::ConnectStatus status;
  fd_ = net::start_connect(host_, port_, status);
  if (fd_ < 0) return false;
  if (status == net::ConnectStatus::InProgress) {
    if (!wait_ready(fd_, POLLOUT, SteadyClock::now() + timeout_) || net::socket_error(fd_) != 0) {
      close();
      return false;
    }
  }
  return true;
}

bool NodeClient::call(const std::vector<std::vector<std::string>>& commands, std::vector<std::string>& replies) {
  replies.clear();
  if (fd_ < 0 && !connect()) return false;
  const auto deadline = SteadyClock::now() + timeout_;

  std::string out;
  for (const auto& c : commands) out += resp::encode_command(c);
  std::size_t sent = 0;
  while (sent < out.size()) {
    const ssize_t n = ::send(fd_, out.data() + sent, out.size() - sent, net::kSendFlags);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && net::would_block()) {
      if (!wait_ready(fd_, POLLOUT, deadline)) break;
    } else {
      break;
    }
  }
  if (sent < out.size()) {
    close();
    return false;
  }

  while (replies.size() < commands.size()) {
    std::size_t len = 0;
    const auto st = resp::frame_reply(in_, max_reply_bytes_, len);
    if (st == resp::ParseStatus::Ok) {
      replies.push_back(in_.substr(0, len));
      in_.erase(0, len);
      continue;
    }
    if (st == resp::ParseStatus::Error || !wait_ready(fd_, POLLIN, deadline)) {
      close();
      return false;
    }
    const ssize_t n = ::recv(fd_, read_buf_.data(), read_buf_.size(), 0);
    if (n > 0) {
      in_.append(read_buf_.data(), static_cast<std::size_t>(n));
    } else if (n < 0 && (errno == EINTR || net::would_block())) {
      continue;
    } else {
      close();
      return false;
    }
  }
  return true;
}

bool NodeClient::call(const std::vector<std::string>& command, std::string& reply) {
  std::vector<std::string> replies;
  if (!call(std::vector<std::vector<std::string>>{command}, replies)) return false;
  reply = std::move(replies.front());
  return true;
}

}  // namespace cachegrid
