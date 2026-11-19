#include "cachegrid/HashRing.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>

using namespace cachegrid;
using namespace cachegrid::test;

namespace {

const std::size_t kVnodes = cfg_size("test.ring.vnodes");
const int kKeys = cfg_int("test.ring.keys");
const double kBalanceTol = static_cast<double>(cfg_size("test.ring.balance_tolerance_percent")) / 100;
const double kMoveTol = static_cast<double>(cfg_size("test.ring.move_tolerance_percent")) / 100;

std::string key(int i) { return "key:" + std::to_string(i); }

HashRing ring_of(std::initializer_list<const char*> ids, std::size_t vnodes = 0) {
  HashRing r(vnodes == 0 ? kVnodes : vnodes);
  for (const char* id : ids) r.add_node(id);
  return r;
}

}  // namespace

// ---- Hash function -------------------------------------------------------------

TEST(HashRingHash, MatchesReferenceValues) {
  // Computed independently (Python implementation of FNV-1a + Murmur3
  // finalizer). If these change, key placement changes on every deployment.
  EXPECT_EQ(HashRing::hash(""), 17280346270528514342ULL);
  EXPECT_EQ(HashRing::hash("key:1"), 4098048589259694704ULL);
  EXPECT_EQ(HashRing::hash("node1#0"), 17079302933345251961ULL);
}

// ---- Basic behaviour --------------------------------------------------------------

TEST(HashRing, ZeroVnodesRejected) { EXPECT_THROW(HashRing(0), std::invalid_argument); }

TEST(HashRing, EmptyRingHasNoOwner) {
  HashRing r(kVnodes);
  EXPECT_TRUE(r.empty());
  EXPECT_THROW(r.owner("k"), std::logic_error);
}

TEST(HashRing, SingleNodeOwnsEverything) {
  const HashRing r = ring_of({"solo"});
  for (int i = 0; i < kKeys; ++i) ASSERT_EQ(r.owner(key(i)), "solo");
}

TEST(HashRing, DuplicateAddAndUnknownRemoveAreNoOps) {
  HashRing r = ring_of({"a", "b"});
  const std::string before = r.owner("x");
  r.add_node("a");
  r.remove_node("zzz");
  EXPECT_EQ(r.node_count(), 2u);
  EXPECT_EQ(r.owner("x"), before);
}

TEST(HashRing, PlacementIndependentOfInsertionOrder) {
  // Two proxies configured with the same nodes in a different order must
  // route every key identically.
  const HashRing a = ring_of({"node1", "node2", "node3"});
  const HashRing b = ring_of({"node3", "node1", "node2"});
  for (int i = 0; i < kKeys; ++i) ASSERT_EQ(a.owner(key(i)), b.owner(key(i)));
}

// ---- Balance -----------------------------------------------------------------------

TEST(HashRing, VirtualNodesBalanceLoad) {
  const HashRing r = ring_of({"node1", "node2", "node3"});
  std::map<std::string, int> count;
  for (int i = 0; i < kKeys; ++i) ++count[r.owner(key(i))];
  ASSERT_EQ(count.size(), 3u);
  // A node's share varies by roughly 1/sqrt(vnodes), hence a tolerance
  // (test.ring.balance_tolerance_percent). With 1 vnode per node the spread
  // is far worse (next test, and benchmarks/results.md).
  const double ideal = kKeys / 3.0;
  for (const auto& [id, c] : count) {
    EXPECT_NEAR(c, ideal, ideal * kBalanceTol) << id << " is too far off an even share";
  }
}

TEST(HashRing, OneVnodePerNodeIsUneven) {
  // The reason virtual nodes exist: with one point each, arcs are lopsided.
  const HashRing r = ring_of({"node1", "node2", "node3"}, 1);
  std::map<std::string, int> count;
  for (int i = 0; i < kKeys; ++i) ++count[r.owner(key(i))];
  int max_c = 0;
  for (const auto& [id, c] : count) max_c = std::max(max_c, c);
  EXPECT_GT(max_c, kKeys / 3.0 * (1 + kBalanceTol));
}

// ---- Stability: the point of consistent hashing ----------------------------------------

TEST(HashRing, AddingNodeOnlyMovesKeysToTheNewNode) {
  const HashRing before = ring_of({"node1", "node2", "node3"});
  const HashRing after = ring_of({"node1", "node2", "node3", "node4"});
  int moved = 0;
  for (int i = 0; i < kKeys; ++i) {
    const std::string& a = before.owner(key(i));
    const std::string& b = after.owner(key(i));
    if (a != b) {
      ++moved;
      ASSERT_EQ(b, "node4") << "key moved between two old nodes";
    }
  }
  // About 1/4 of keys should move to the new node.
  EXPECT_NEAR(moved, kKeys / 4.0, kKeys / 4.0 * kMoveTol);
}

TEST(HashRing, RemovingNodeOnlyMovesThatNodesKeys) {
  const HashRing before = ring_of({"node1", "node2", "node3"});
  const HashRing after = ring_of({"node1", "node3"});
  for (int i = 0; i < kKeys; ++i) {
    const std::string& a = before.owner(key(i));
    if (a != "node2") {
      ASSERT_EQ(after.owner(key(i)), a) << "key moved although its node stayed";
    }
  }
}

TEST(HashRing, RemoveThenReAddRestoresPlacement) {
  const HashRing original = ring_of({"node1", "node2", "node3"});
  HashRing r = ring_of({"node1", "node2", "node3"});
  r.remove_node("node2");
  r.add_node("node2");
  for (int i = 0; i < kKeys; ++i) ASSERT_EQ(r.owner(key(i)), original.owner(key(i)));
}
