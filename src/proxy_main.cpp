// cachegrid-proxy: routes client commands to cache nodes by consistent hashing.
//
//   ./build/release/cachegrid-proxy config/proxy.conf
//
// Signals:
//   SIGINT / SIGTERM  stop cleanly
//   SIGHUP            re-read the config file and apply the new `proxy.nodes`
//                     list (add/remove nodes without restarting). Other
//                     settings need a restart.

#include <pthread.h>

#include <csignal>
#include <cstdio>

#include "cachegrid/Config.h"
#include "cachegrid/Net.h"
#include "cachegrid/ProxyServer.h"

using namespace cachegrid;

namespace {

void print_nodes(const std::vector<NodeAddress>& nodes) {
  for (const auto& n : nodes) std::printf("  node %s\n", n.to_string().c_str());
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <config-file>\n", argv[0]);
    return 2;
  }
  const std::string config_path = argv[1];

  try {
    const ProxyOptions opts = ProxyOptions::from_config(Config::load_file(config_path));
    net::ensure_fd_limit(opts.net.max_connections + opts.net.fd_reserve);

    // Same signal approach as the node: block, then handle synchronously.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    ProxyServer proxy(opts);
    proxy.start();
    std::printf("cachegrid-proxy listening on %s:%u | %zu workers, %zu vnodes per node\n",
                opts.net.bind_address.c_str(), proxy.port(), opts.net.worker_threads, opts.vnodes_per_node);
    print_nodes(opts.nodes);

    for (;;) {
      int sig = 0;
      sigwait(&sigs, &sig);
      if (sig != SIGHUP) {
        std::printf("received %s, shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
        break;
      }
      // A bad edit must not take the running proxy down: keep the old
      // membership and report the error.
      try {
        const ProxyOptions fresh = ProxyOptions::from_config(Config::load_file(config_path));
        proxy.set_nodes(fresh.nodes);
        std::printf("SIGHUP: membership reloaded (%zu nodes)\n", fresh.nodes.size());
        print_nodes(fresh.nodes);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "SIGHUP: reload failed, keeping current nodes: %s\n", e.what());
      }
    }
    proxy.stop();
    return 0;
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
  }
  return 1;
}
