// cachegrid-bench: load tester for a CacheGrid node or proxy.
//
//   ./build/release/cachegrid-bench config/bench/<scenario>.conf
//
// Every setting comes from the scenario config (which includes
// config/bench.conf). Prints a Markdown report to stdout.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "cachegrid/Config.h"
#include "cachegrid/LoadGenerator.h"
#include "cachegrid/Net.h"
#include "cachegrid/NodeClient.h"
#include "cachegrid/Resp.h"
#include "cachegrid/Workload.h"

using namespace cachegrid;

namespace {

constexpr double kNsPerMs = 1e6;  // unit conversion

std::string ms(std::uint64_t ns) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(ns) / kNsPerMs);
  return buf;
}

std::size_t parse_count(const std::string& s, const std::string& key) {
  return Config::parse("v = " + s, key).get_size("v");
}

struct NodeStats {
  std::uint64_t evicted = 0, used_memory = 0, keys = 0, peak_rss = 0;
  double cpu_seconds = 0;
  bool ok = false;
};

// Reads eviction/memory counters from a node's INFO.
NodeStats node_stats(const std::string& host_port, const LoadOptions& o) {
  NodeStats s;
  const auto colon = host_port.rfind(':');
  if (colon == std::string::npos) return s;
  NodeClient c(host_port.substr(0, colon), static_cast<std::uint16_t>(std::stoul(host_port.substr(colon + 1))),
               o.connect_timeout, o.max_reply_bytes, o.read_chunk_bytes);
  std::string raw;
  resp::Value v;
  if (!c.call({"INFO"}, raw) || !resp::decode_reply(raw, v) || v.type != resp::Value::Type::Bulk) return s;
  auto field = [&](const std::string& name) -> std::string {
    const auto p = v.str.find("\n" + name + ":");
    if (p == std::string::npos) return "0";
    const auto start = p + name.size() + 2;
    return v.str.substr(start, v.str.find('\r', start) - start);
  };
  s.evicted = std::stoull(field("evicted_keys"));
  s.used_memory = std::stoull(field("used_memory"));
  s.keys = std::stoull(field("keys"));
  s.peak_rss = std::stoull(field("peak_rss_bytes"));
  s.cpu_seconds = std::stod(field("used_cpu_seconds"));
  s.ok = true;
  return s;
}

// "none" in the list = nothing to query (e.g. no proxy in this scenario).
NodeStats total_stats(const std::vector<std::string>& nodes, const LoadOptions& o) {
  NodeStats t;
  t.ok = true;
  if (nodes.size() == 1 && nodes[0] == "none") return t;
  for (const auto& n : nodes) {
    const NodeStats s = node_stats(n, o);
    t.ok = t.ok && s.ok;
    t.evicted += s.evicted;
    t.used_memory += s.used_memory;
    t.keys += s.keys;
    t.peak_rss += s.peak_rss;
    t.cpu_seconds += s.cpu_seconds;
  }
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario.conf>\n", argv[0]);
    return 2;
  }
  try {
    const Config cfg = Config::load_file(argv[1]);
    const auto workload = make_workload_from_config(cfg);
    LoadOptions base = load_options_from_config(cfg);
    const std::size_t max_conns = cfg.get_size("bench.load.max_connections");
    net::ensure_fd_limit(max_conns + cfg.get_size("bench.load.fd_reserve"));
    std::vector<std::size_t> levels;
    for (const auto& item : cfg.get_list("bench.load.concurrency_levels")) {
      levels.push_back(parse_count(item, "bench.load.concurrency_levels"));
      if (levels.back() == 0) throw ConfigError("'bench.load.concurrency_levels' entries must be >= 1");
    }
    const auto stats_nodes = cfg.get_list("bench.load.stats_nodes");
    const auto stats_proxy = cfg.get_list("bench.load.stats_proxy");

    std::printf("## %s\n\n", cfg.get_string("bench.scenario.name").c_str());
    std::printf("- **Target:** %s:%u\n", base.host.c_str(), base.port);
    std::printf("- **Workload:** %s\n", workload->describe().c_str());
    std::printf("- **Client:** %zu threads, up to %zu connections; warmup %lld ms, measured %lld ms per level\n",
                base.threads, max_conns, static_cast<long long>(base.warmup.count()),
                static_cast<long long>(base.duration.count()));
    std::printf("- **On a GET miss:** %s\n", base.refill_on_miss
                                                  ? "the client SETs the key back (cache-aside, like a real application)"
                                                  : "nothing (misses stay misses)");

    if (cfg.get_bool("bench.workload.preload")) {
      LoadOptions p = base;
      p.connections = cfg.get_size("bench.load.preload_connections");
      p.depth = cfg.get_size("bench.load.preload_depth");
      const auto ops = workload->preload_ops();
      const std::uint64_t errors = run_ops_once(p, *workload, ops);
      std::printf("- **Preload:** %zu keys written before measuring (%llu errors)\n", ops.size(),
                  static_cast<unsigned long long>(errors));
    }
    std::printf("\nLatencies in milliseconds. Concurrency = connections x requests in flight per connection.\n\n");
    std::printf("CPU %% = cores busy x 100 (200%% = two cores), measured over the level incl. warmup.\n\n");
    std::printf("| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | "
                "SET p95 | Hit %% | Errors | Evictions | Nodes CPU %% | Proxy CPU %% | Client CPU %% |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    std::fflush(stdout);

    std::vector<LoadResult> results;
    std::string refill_note;
    for (std::size_t level : levels) {
      LoadOptions o = base;
      o.connections = std::min(level, max_conns);
      o.depth = (level + o.connections - 1) / o.connections;  // round up
      const NodeStats before = total_stats(stats_nodes, o);
      const NodeStats proxy_before = total_stats(stats_proxy, o);
      const auto t0 = std::chrono::steady_clock::now();
      LoadResult r = run_load(o, *workload);
      const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      const NodeStats after = total_stats(stats_nodes, o);
      const NodeStats proxy_after = total_stats(stats_proxy, o);
      auto server_cpu = [&](const NodeStats& b, const NodeStats& a, bool present) {
        if (!present) return std::string("-");
        if (!b.ok || !a.ok) return std::string("n/a");
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f", 100.0 * (a.cpu_seconds - b.cpu_seconds) / wall);
        return std::string(buf);
      };
      const bool has_proxy = !(stats_proxy.size() == 1 && stats_proxy[0] == "none");

      const double hit = r.hits + r.misses ? 100.0 * static_cast<double>(r.hits) /
                                                 static_cast<double>(r.hits + r.misses)
                                           : 0.0;
      const double cpu = r.seconds > 0 ? 100.0 * r.client_cpu_seconds / r.seconds : 0.0;
      const std::string evictions =
          before.ok && after.ok ? std::to_string(after.evicted - before.evicted) : std::string("n/a");
      std::printf("| %zu | %zu x %zu | %.0f | %s | %s | %s | %s | %s | %.1f | %llu | %s | %s | %s | %.0f |\n",
                  o.connections * o.depth, o.connections, o.depth, r.ops_per_sec(), ms(r.get.percentile(50)).c_str(),
                  ms(r.get.percentile(95)).c_str(), ms(r.get.percentile(99)).c_str(), ms(r.get.max()).c_str(),
                  ms(r.set.percentile(95)).c_str(), hit, static_cast<unsigned long long>(r.errors + r.connections_failed),
                  evictions.c_str(), server_cpu(before, after, true).c_str(),
                  server_cpu(proxy_before, proxy_after, has_proxy).c_str(), cpu);
      std::fflush(stdout);
      if (r.refills) refill_note += " " + std::to_string(level) + ": " + std::to_string(r.refills) + ";";
      results.push_back(std::move(r));
    }

    if (!refill_note.empty()) {
      std::printf("\nCache-aside refills (SETs after a miss, included in throughput) per level:%s\n",
                  refill_note.c_str());
    }
    constexpr double kBytesPerMb = 1024.0 * 1024.0;  // unit conversion
    const NodeStats final_stats = total_stats(stats_nodes, base);
    const NodeStats final_proxy = total_stats(stats_proxy, base);
    if (final_stats.ok) {
      std::printf("\n**Memory:** nodes hold %llu keys (all copies) using %.1f MB of cache data; "
                  "peak process memory (RSS) across nodes %.1f MB",
                  static_cast<unsigned long long>(final_stats.keys),
                  static_cast<double>(final_stats.used_memory) / kBytesPerMb,
                  static_cast<double>(final_stats.peak_rss) / kBytesPerMb);
      if (final_proxy.ok && final_proxy.peak_rss) {
        std::printf(", proxy %.1f MB", static_cast<double>(final_proxy.peak_rss) / kBytesPerMb);
      }
      std::printf(".\n");
    }

    // Per-interval timeline (e.g. to watch a node failure and recovery).
    if (base.timeline_interval.count() > 0) {
      for (std::size_t i = 0; i < results.size(); ++i) {
        std::printf("\n### Timeline at concurrency %zu (every %lld ms)\n\n", levels[i],
                    static_cast<long long>(base.timeline_interval.count()));
        std::printf("| t (s) | Throughput (ops/s) | p50 | p95 | p99 | max | Errors |\n|---:|---:|---:|---:|---:|---:|---:|\n");
        const double secs = static_cast<double>(base.timeline_interval.count()) / 1000.0;
        // Only whole intervals inside the measured window (the last bucket can
        // catch a few replies that arrive right as the run stops).
        const auto whole = static_cast<std::size_t>(base.duration.count() / base.timeline_interval.count());
        for (std::size_t t = 0; t < std::min(whole, results[i].timeline.size()); ++t) {
          const IntervalResult& r = results[i].timeline[t];
          std::printf("| %.1f | %.0f | %s | %s | %s | %s | %llu |\n", static_cast<double>(t) * secs,
                      static_cast<double>(r.ops) / secs, ms(r.latency.percentile(50)).c_str(),
                      ms(r.latency.percentile(95)).c_str(), ms(r.latency.percentile(99)).c_str(),
                      ms(r.latency.max()).c_str(), static_cast<unsigned long long>(r.errors));
        }
      }
    }
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
