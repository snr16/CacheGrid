#include "cachegrid/HashRing.h"

#include <algorithm>
#include <stdexcept>

namespace cachegrid {

std::uint64_t HashRing::hash(std::string_view data) {
  // FNV-1a: a tiny, well-known hash. Its published constants are part of the
  // algorithm's definition, not tunables.
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
  std::uint64_t h = kFnvOffset;
  for (unsigned char c : data) {
    h ^= c;
    h *= kFnvPrime;
  }
  // FNV alone mixes the last bytes poorly ("node1#1" vs "node1#2" land close
  // together). MurmurHash3's 64-bit finalizer scrambles every input bit into
  // every output bit, giving well-spread ring positions.
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

HashRing::HashRing(std::size_t vnodes_per_node) : vnodes_per_node_(vnodes_per_node) {
  if (vnodes_per_node == 0) throw std::invalid_argument("vnodes_per_node must be >= 1");
}

void HashRing::add_node(const std::string& node_id) {
  auto it = std::lower_bound(nodes_.begin(), nodes_.end(), node_id);
  if (it != nodes_.end() && *it == node_id) return;
  nodes_.insert(it, node_id);
  rebuild();
}

void HashRing::remove_node(const std::string& node_id) {
  auto it = std::lower_bound(nodes_.begin(), nodes_.end(), node_id);
  if (it == nodes_.end() || *it != node_id) return;
  nodes_.erase(it);
  rebuild();
}

void HashRing::rebuild() {
  // Positions depend only on node ids, never on insertion order, so every
  // proxy with the same membership builds an identical ring.
  ring_.clear();
  ring_.reserve(nodes_.size() * vnodes_per_node_);
  for (std::uint32_t n = 0; n < nodes_.size(); ++n) {
    for (std::size_t v = 0; v < vnodes_per_node_; ++v) {
      ring_.emplace_back(hash(nodes_[n] + "#" + std::to_string(v)), n);
    }
  }
  // Sorting by (position, node index) also breaks the (astronomically rare)
  // tie of two vnodes hashing to the same position deterministically.
  std::sort(ring_.begin(), ring_.end());
}

const std::string& HashRing::owner(std::string_view key) const {
  if (ring_.empty()) throw std::logic_error("HashRing::owner on empty ring");
  const std::uint64_t h = hash(key);
  // First point at or after h, i.e. "clockwise"...
  auto it = std::lower_bound(ring_.begin(), ring_.end(), std::make_pair(h, std::uint32_t{0}));
  // ...wrapping past the top of the circle back to the start.
  if (it == ring_.end()) it = ring_.begin();
  return nodes_[it->second];
}

void HashRing::owner_positions(std::string_view key, std::size_t n, std::vector<std::uint32_t>& out) const {
  if (ring_.empty()) throw std::logic_error("HashRing::owners on empty ring");
  out.clear();
  const std::size_t want = std::min(n, nodes_.size());
  auto it = std::lower_bound(ring_.begin(), ring_.end(), std::make_pair(hash(key), std::uint32_t{0}));
  for (std::size_t steps = 0; out.size() < want && steps < ring_.size(); ++steps, ++it) {
    if (it == ring_.end()) it = ring_.begin();
    // `out` holds at most replication_factor entries, so a linear scan for
    // "already chosen" is faster than any set.
    if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
  }
}

std::vector<std::string> HashRing::owners(std::string_view key, std::size_t n) const {
  std::vector<std::uint32_t> pos;
  owner_positions(key, n, pos);
  std::vector<std::string> out;
  for (std::uint32_t p : pos) out.push_back(nodes_[p]);
  return out;
}

}  // namespace cachegrid
