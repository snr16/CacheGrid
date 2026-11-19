// Measures the two properties that make consistent hashing worth it:
//   1. Balance: how evenly keys spread over nodes, per virtual-node count.
//   2. Stability: how many keys change owner when a node joins or leaves,
//      compared with naive `hash % N`.
//
//   ./build/release/ring_analysis config/bench.conf

#include "cachegrid/Config.h"
#include "cachegrid/HashRing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace cachegrid;

namespace {

std::vector<std::string> make_ids(std::size_t n) {
  std::vector<std::string> ids;
  for (std::size_t i = 1; i <= n; ++i) ids.push_back("node" + std::to_string(i));
  return ids;
}

HashRing make_ring(const std::vector<std::string>& ids, std::size_t vnodes) {
  HashRing r(vnodes);
  for (const auto& id : ids) r.add_node(id);
  return r;
}

void balance(const std::vector<std::string>& keys, std::size_t nodes, const std::vector<std::size_t>& vnode_counts) {
  std::printf("\n== Balance: %zu keys over %zu nodes (ideal %.2f%% each) ==\n", keys.size(), nodes,
              100.0 / static_cast<double>(nodes));
  std::printf("%8s %10s %10s %12s %10s\n", "vnodes", "min %", "max %", "max/ideal", "stddev %");
  const auto ids = make_ids(nodes);
  for (std::size_t v : vnode_counts) {
    const HashRing ring = make_ring(ids, v);
    std::map<std::string, std::size_t> count;
    for (const auto& k : keys) ++count[ring.owner(k)];

    const double ideal = static_cast<double>(keys.size()) / static_cast<double>(nodes);
    double min_c = static_cast<double>(keys.size()), max_c = 0, var = 0;
    for (const auto& id : ids) {
      const double c = static_cast<double>(count[id]);
      min_c = std::min(min_c, c);
      max_c = std::max(max_c, c);
      var += (c - ideal) * (c - ideal);
    }
    const double total = static_cast<double>(keys.size());
    std::printf("%8zu %10.2f %10.2f %12.3f %10.2f\n", v, 100 * min_c / total, 100 * max_c / total, max_c / ideal,
                100 * std::sqrt(var / static_cast<double>(nodes)) / total);
  }
}

void stability(const std::vector<std::string>& keys, std::size_t nodes, std::size_t vnodes) {
  std::printf("\n== Membership changes: %zu keys, %zu nodes, %zu vnodes ==\n", keys.size(), nodes, vnodes);
  const double total = static_cast<double>(keys.size());

  // Add one node.
  {
    const auto before_ids = make_ids(nodes);
    const auto after_ids = make_ids(nodes + 1);
    const HashRing before = make_ring(before_ids, vnodes);
    const HashRing after = make_ring(after_ids, vnodes);
    const std::string& added = after_ids.back();
    std::size_t moved = 0, moved_elsewhere = 0;
    for (const auto& k : keys) {
      const std::string& a = before.owner(k);
      const std::string& b = after.owner(k);
      if (a != b) {
        ++moved;
        if (b != added) ++moved_elsewhere;
      }
    }
    std::printf("add %-6s   ring: %6.2f%% of keys moved (ideal %.2f%%); moved to a node other than the new one: %zu\n",
                added.c_str(), 100 * static_cast<double>(moved) / total, 100.0 / static_cast<double>(nodes + 1),
                moved_elsewhere);
  }

  // Remove one node.
  {
    const auto before_ids = make_ids(nodes);
    std::vector<std::string> after_ids(before_ids.begin() + 1, before_ids.end());
    const std::string removed = before_ids.front();
    const HashRing before = make_ring(before_ids, vnodes);
    const HashRing after = make_ring(after_ids, vnodes);
    std::size_t moved = 0, owned_by_removed = 0, moved_wrongly = 0;
    for (const auto& k : keys) {
      const std::string& a = before.owner(k);
      if (a == removed) ++owned_by_removed;
      if (a != after.owner(k)) {
        ++moved;
        if (a != removed) ++moved_wrongly;
      }
    }
    std::printf("remove %-6s ring: %6.2f%% of keys moved (exactly the removed node's share: %.2f%%); "
                "keys moved that weren't on it: %zu\n",
                removed.c_str(), 100 * static_cast<double>(moved) / total,
                100 * static_cast<double>(owned_by_removed) / total, moved_wrongly);
  }

  // Naive modulo hashing for comparison.
  {
    std::size_t moved = 0;
    for (const auto& k : keys) {
      const std::uint64_t h = HashRing::hash(k);
      if (h % nodes != h % (nodes + 1)) ++moved;
    }
    std::printf("add node   hash %% N: %6.2f%% of keys moved\n", 100 * static_cast<double>(moved) / total);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <bench.conf>\n", argv[0]);
    return 2;
  }
  try {
    const Config cfg = Config::load_file(argv[1]);
    const std::size_t key_count = cfg.get_size("bench.ring.keys");
    const std::size_t nodes = cfg.get_size("bench.ring.nodes");
    const std::size_t change_vnodes = cfg.get_size("bench.ring.change_vnodes");
    std::vector<std::size_t> vnode_counts;
    for (const auto& item : cfg.get_list("bench.ring.vnode_counts")) {
      vnode_counts.push_back(Config::parse("v=" + item).get_size("v"));
    }
    if (key_count == 0 || nodes < 2 || change_vnodes == 0) {
      throw ConfigError("bench.ring.keys >= 1, bench.ring.nodes >= 2, bench.ring.change_vnodes >= 1 required");
    }
    for (std::size_t v : vnode_counts) {
      if (v == 0) throw ConfigError("'bench.ring.vnode_counts' entries must be >= 1");
    }

    std::vector<std::string> keys;
    keys.reserve(key_count);
    for (std::size_t i = 0; i < key_count; ++i) keys.push_back("key:" + std::to_string(i));

    balance(keys, nodes, vnode_counts);
    stability(keys, nodes, change_vnodes);
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 1;
  }
  return 0;
}
