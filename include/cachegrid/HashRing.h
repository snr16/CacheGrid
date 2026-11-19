#pragma once

// Consistent hashing with virtual nodes.
//
// Picture a circle of 2^64 positions. Each node is placed on it at many
// pseudo-random points ("virtual nodes"), and each key is hashed onto it too.
// A key belongs to the first node point found clockwise from the key.
//
// Why this beats `hash(key) % N`:
//   - With modulo, changing N from 3 to 4 changes the owner of ~75% of keys
//     (every cached value would have to be refetched).
//   - With a ring, adding a 4th node only moves the keys that land on the new
//     node's points: ~1/4 of keys, all of them going TO the new node. Removing
//     a node only moves that node's keys.
//
// Why virtual nodes: with one point per node the arcs between points are very
// uneven, so one node might own 50% of the keys. Many points per node average
// out the arc lengths, so load is close to 1/N each.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cachegrid {

class HashRing {
 public:
  explicit HashRing(std::size_t vnodes_per_node);

  // Adds a node (no-op if already present). O(V log V) where V = all vnodes.
  void add_node(const std::string& node_id);

  // Removes a node (no-op if absent).
  void remove_node(const std::string& node_id);

  // The node that owns `key`. Ring must not be empty. O(log V).
  const std::string& owner(std::string_view key) const;

  // The first `n` *distinct* nodes found walking clockwise from `key`: the
  // owner first, then its replicas (fewer if the ring has fewer nodes).
  // Virtual nodes of an already-chosen node are skipped, so replicas always
  // land on different machines.
  std::vector<std::string> owners(std::string_view key, std::size_t n) const;

  bool empty() const { return nodes_.empty(); }
  std::size_t node_count() const { return nodes_.size(); }
  const std::vector<std::string>& nodes() const { return nodes_; }
  std::size_t vnodes_per_node() const { return vnodes_per_node_; }

  // 64-bit hash used for both keys and virtual-node positions.
  // Deterministic on every platform and compiler (unlike std::hash), so all
  // proxies, tests and tools agree on key placement. Deliberately different
  // from the per-node shard hash (std::hash), so keys sent to one node still
  // spread evenly across its shards.
  static std::uint64_t hash(std::string_view data);

 private:
  void rebuild();

  std::size_t vnodes_per_node_;
  std::vector<std::string> nodes_;                       // sorted node ids
  std::vector<std::pair<std::uint64_t, std::uint32_t>> ring_;  // (position, index into nodes_)
};

}  // namespace cachegrid
