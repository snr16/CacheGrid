#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cachegrid/Cache.h"
#include "cachegrid/Net.h"

namespace cachegrid {

class Config;

// All values come from the `server.*` keys in config/cachegrid.conf.
struct ServerOptions {
  ListenerOptions net;    // port, threads, limits (shared with the proxy)
  Millis purge_interval;  // how often expired keys are swept

  static ServerOptions from_config(const Config& cfg);
};

// TCP front end for a Cache.
//
// Threading model (same idea as Memcached):
//   - `worker_threads` threads, each running its own event loop (Poller).
//   - All workers watch the shared listening socket; whichever wakes first
//     accepts the new client and serves it for the rest of its life.
//   - Each worker reads requests, runs them against the (thread-safe) Cache
//     directly and writes replies. No hand-off to another pool: a cache
//     operation takes well under a microsecond, so passing it to another
//     thread would cost more than doing the work.
//   - One extra thread sweeps expired keys every `purge_interval`.
class CacheServer {
 public:
  CacheServer(ServerOptions opts, Cache& cache);
  ~CacheServer();
  CacheServer(const CacheServer&) = delete;
  CacheServer& operator=(const CacheServer&) = delete;

  // Binds, listens and starts all threads. Throws std::system_error on failure.
  void start();

  // Stops accepting, closes every connection and joins all threads.
  // Safe to call more than once.
  void stop();

  // The actual listening port (useful when configured with port 0).
  std::uint16_t port() const { return bound_port_; }

  std::size_t active_connections() const { return active_.load(); }

 private:
  class Worker;
  friend class Worker;

  void purge_loop();
  void append_info(std::string& out) const;

  ServerOptions opts_;
  Cache& cache_;
  int listen_fd_ = -1;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stopping_{false};
  bool started_ = false;

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;

  std::thread purge_thread_;
  std::mutex purge_mu_;
  std::condition_variable purge_cv_;

  // Stats (atomics: updated by every worker thread).
  std::atomic<std::size_t> active_{0};
  std::atomic<std::uint64_t> total_accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> commands_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
};

}  // namespace cachegrid
