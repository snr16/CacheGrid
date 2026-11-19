// cachegrid-server: runs one cache node.
//
//   ./build/release/cachegrid-server config/cachegrid.conf
//
// Stops cleanly on Ctrl-C (SIGINT) or SIGTERM (what `docker stop` sends).

#include <pthread.h>
#include <sys/resource.h>

#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "cachegrid/Cache.h"
#include "cachegrid/CacheServer.h"
#include "cachegrid/Config.h"

using namespace cachegrid;

namespace {

// Every connection is a file descriptor, and the OS caps how many a process
// may open ("ulimit -n"). Raise our soft limit if needed, or refuse to start:
// hitting the cap at runtime makes accept() fail in a tight loop.
void ensure_fd_limit(std::size_t needed) {
  rlimit rl{};
  if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
  if (rl.rlim_cur >= needed) return;

  rlimit want = rl;
  want.rlim_cur = static_cast<rlim_t>(needed);
  const bool hard_ok = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= needed;
  if (!hard_ok || ::setrlimit(RLIMIT_NOFILE, &want) != 0) {
    throw std::runtime_error("server.max_connections + server.fd_reserve = " + std::to_string(needed) +
                             " exceeds the open-file limit (" + std::to_string(rl.rlim_cur) +
                             "); lower server.max_connections or raise `ulimit -n`");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <config-file>\n", argv[0]);
    return 2;
  }

  try {
    const Config cfg = Config::load_file(argv[1]);
    const Cache::Options cache_opts = Cache::Options::from_config(cfg);
    const ServerOptions server_opts = ServerOptions::from_config(cfg);
    ensure_fd_limit(server_opts.max_connections + server_opts.fd_reserve);

    // Block SIGINT/SIGTERM in every thread (threads inherit this mask), then
    // wait for them synchronously below with sigwait(). This avoids running
    // shutdown code inside an async signal handler, where almost nothing is
    // safe to call.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // belt-and-braces; sends already avoid SIGPIPE

    Cache cache(cache_opts);
    CacheServer server(server_opts, cache);
    server.start();
    std::printf("cachegrid listening on %s:%u | %zu workers, %zu shards, %zu bytes capacity\n",
                server_opts.bind_address.c_str(), server.port(), server_opts.worker_threads,
                cache.num_shards(), cache.capacity_bytes());
    std::fflush(stdout);

    int sig = 0;
    sigwait(&sigs, &sig);
    std::printf("received %s, shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
    server.stop();
    return 0;
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
  }
  return 1;
}
