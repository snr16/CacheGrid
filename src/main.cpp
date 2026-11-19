// cachegrid-server: runs one cache node.
//
//   ./build/release/cachegrid-server config/cachegrid.conf
//
// Stops cleanly on Ctrl-C (SIGINT) or SIGTERM (what `docker stop` sends).

#include <pthread.h>

#include <csignal>
#include <cstdio>

#include "cachegrid/Cache.h"
#include "cachegrid/CacheServer.h"
#include "cachegrid/Config.h"
#include "cachegrid/Net.h"

using namespace cachegrid;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <config-file>\n", argv[0]);
    return 2;
  }

  try {
    const Config cfg = Config::load_file(argv[1]);
    const Cache::Options cache_opts = Cache::Options::from_config(cfg);
    const ServerOptions server_opts = ServerOptions::from_config(cfg);
    net::ensure_fd_limit(server_opts.net.max_connections + server_opts.net.fd_reserve);

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
                server_opts.net.bind_address.c_str(), server.port(), server_opts.net.worker_threads,
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
