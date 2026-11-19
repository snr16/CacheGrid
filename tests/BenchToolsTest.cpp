// Tests for the load-testing tools: histogram, workloads, load generator.

#include "cachegrid/LatencyHistogram.h"
#include "cachegrid/LoadGenerator.h"
#include "cachegrid/Workload.h"
#include "TestNet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <sstream>

using namespace cachegrid;
using namespace cachegrid::test;

namespace {
unsigned hist_bits() { return static_cast<unsigned>(cfg_size("test.histogram.bits")); }
}  // namespace

// ---- LatencyHistogram ------------------------------------------------------------

TEST(LatencyHistogram, SmallValuesAreExact) {
  LatencyHistogram h(hist_bits());
  for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
  EXPECT_EQ(h.count(), 100u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 100u);
  EXPECT_EQ(h.percentile(50), 50u);
  EXPECT_EQ(h.percentile(95), 95u);
  EXPECT_EQ(h.percentile(100), 100u);
  EXPECT_DOUBLE_EQ(h.mean(), 50.5);
}

TEST(LatencyHistogram, PercentilesWithinPrecisionOfExactAnswer) {
  const unsigned bits = hist_bits();
  const double max_err = static_cast<double>(cfg_size("test.histogram.max_error_percent")) / 100;
  LatencyHistogram h(bits);
  std::mt19937_64 rng(cfg_size("test.random_seed"));
  // Log-uniform latencies from ~1us to ~1s (in ns), like real tails.
  std::uniform_real_distribution<double> exp(3.0, 9.0);
  std::vector<std::uint64_t> samples;
  for (std::size_t i = 0; i < cfg_size("test.histogram.samples"); ++i) {
    samples.push_back(static_cast<std::uint64_t>(std::pow(10.0, exp(rng))));
    h.record(samples.back());
  }
  std::sort(samples.begin(), samples.end());
  for (double p : {50.0, 90.0, 95.0, 99.0, 99.9}) {
    const auto exact = samples[static_cast<std::size_t>(std::ceil(p / 100 * static_cast<double>(samples.size()))) - 1];
    const auto got = h.percentile(p);
    EXPECT_GE(got, exact) << "p" << p << " must never under-report";
    EXPECT_LE(static_cast<double>(got), static_cast<double>(exact) * (1 + max_err)) << "p" << p;
  }
}

TEST(LatencyHistogram, MergeEqualsRecordingEverything) {
  const unsigned bits = hist_bits();
  LatencyHistogram a(bits), b(bits), all(bits);
  for (std::uint64_t v = 1; v < 5000; v += 7) {
    (v % 2 ? a : b).record(v * 1000);
    all.record(v * 1000);
  }
  a.merge(b);
  EXPECT_EQ(a.count(), all.count());
  EXPECT_EQ(a.percentile(95), all.percentile(95));
  EXPECT_EQ(a.max(), all.max());
  EXPECT_THROW(a.merge(LatencyHistogram(bits + 1)), std::invalid_argument);
}

TEST(LatencyHistogram, EmptyAndHugeValues) {
  LatencyHistogram h(hist_bits());
  EXPECT_EQ(h.percentile(99), 0u);
  h.record(std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(h.percentile(99), std::numeric_limits<std::uint64_t>::max());
}

// ---- Workloads -----------------------------------------------------------------

TEST(Zipfian, IsSkewedTowardLowRanks) {
  const std::size_t n = cfg_size("test.zipf.keys");
  ZipfianGenerator z(n, static_cast<double>(cfg_size("test.zipf.theta_percent")) / 100);
  std::mt19937_64 rng(cfg_size("test.random_seed"));
  std::vector<std::size_t> count(n, 0);
  const std::size_t samples = cfg_size("test.zipf.samples");
  for (std::size_t i = 0; i < samples; ++i) {
    const auto r = z.next(rng);
    ASSERT_LT(r, n);
    ++count[r];
  }
  EXPECT_GT(count[0], count[1]);
  EXPECT_GT(count[1], count[n / 2]);
  std::size_t top = 0;
  for (std::size_t i = 0; i < n / 100; ++i) top += count[i];
  EXPECT_GT(100.0 * static_cast<double>(top) / static_cast<double>(samples),
            static_cast<double>(cfg_size("test.zipf.min_top1_percent_share")));
}

TEST(SyntheticWorkload, RespectsMixAndKeySpace) {
  SyntheticOptions o{SyntheticOptions::Distribution::Uniform, 100, 16, 90, 0.0};
  auto w = make_synthetic_workload(o);
  std::mt19937_64 rng(cfg_size("test.random_seed"));
  int gets = 0;
  const int n = 10000;
  for (int i = 0; i < n; ++i) {
    const Op op = w->next(i, rng);
    ASSERT_LT(op.key, 100u);
    gets += op.type == Op::Type::Get;
  }
  EXPECT_NEAR(gets, n * 0.9, n * 0.02);
  EXPECT_EQ(w->preload_ops().size(), 100u);
  EXPECT_EQ(w->key_name(7), "key:7");
}

TEST(TraceWorkload, MapsOperationsAndInternsKeys) {
  std::istringstream trace(
      "0,a,1,10,1,get,0\n"
      "0,b,1,20,1,set,3600\n"
      "1,a,1,30,2,gets,0\n"
      "1,c,1,5,3,cas,60\n"
      "2,b,1,0,1,delete,0\n"
      "2,d,1,999999,1,add,0\n"
      "3,e,1,1,1,weird,0\n"
      "bad line\n");
  TraceStats st;
  auto w = load_trace_workload(trace, 100, 1000, "test", st);
  EXPECT_EQ(st.lines, 8u);
  EXPECT_EQ(st.gets, 2u);
  EXPECT_EQ(st.sets, 3u);
  EXPECT_EQ(st.dels, 1u);
  EXPECT_EQ(st.skipped, 2u);
  EXPECT_EQ(st.clamped, 1u);
  EXPECT_EQ(w->key_count(), 4u);  // a, b, c, d

  std::mt19937_64 rng(0);
  const Op set_b = w->next(1, rng);
  EXPECT_EQ(set_b.type, Op::Type::Set);
  EXPECT_EQ(w->key_name(set_b.key), "b");
  EXPECT_EQ(set_b.ttl_s, 3600u);
  EXPECT_EQ(w->next(4, rng).type, Op::Type::Del);
  EXPECT_EQ(w->next(5, rng).value_size, 1000u) << "clamped";
  EXPECT_EQ(w->next(6, rng).type, Op::Type::Get) << "replay wraps around";
  EXPECT_EQ(w->max_value_size(), 1000u);
  // Preload sets each key at the largest size the trace shows for it.
  const auto pre = w->preload_ops();
  ASSERT_EQ(pre.size(), 4u);
  EXPECT_EQ(pre[0].value_size, 30u);  // "a": 10 then 30
}

// ---- Load generator end to end ---------------------------------------------------

namespace {
LoadOptions test_load_opts(std::uint16_t port) {
  LoadOptions o{};
  o.host = "127.0.0.1";
  o.port = port;
  o.threads = cfg_size("test.load.threads");
  o.connections = cfg_size("test.load.connections");
  o.depth = cfg_size("test.load.depth");
  o.warmup = cfg_ms("test.load.warmup_ms");
  o.duration = cfg_ms("test.load.duration_ms");
  o.connect_timeout = cfg_ms("test.client.recv_timeout_ms");
  o.poll_tick = cfg_ms("test.load.poll_tick_ms");
  o.timeline_interval = cfg_ms("test.load.duration_ms") / 4;
  o.read_chunk_bytes = cfg_bytes("test.listener.read_chunk_bytes");
  o.events_per_poll = cfg_size("test.listener.events_per_poll");
  o.max_reply_bytes = cfg_bytes("test.proxy.max_reply_bytes");
  o.histogram_bits = hist_bits();
  o.seed = cfg_size("test.random_seed");
  o.refill_on_miss = true;
  return o;
}
}  // namespace

TEST(LoadGenerator, PreloadThenMeasureAgainstRealServer) {
  TestServer s;
  SyntheticOptions so{SyntheticOptions::Distribution::Uniform, cfg_size("test.load.keys"),
                      static_cast<std::uint32_t>(cfg_size("test.load.value_size")), 90, 0.0};
  auto w = make_synthetic_workload(so);
  const LoadOptions o = test_load_opts(s.port());

  EXPECT_EQ(run_ops_once(o, *w, w->preload_ops()), 0u);
  EXPECT_EQ(s.cache.size(), cfg_size("test.load.keys")) << "every key preloaded";

  const LoadResult r = run_load(o, *w);
  EXPECT_GT(r.ops, 0u);
  EXPECT_EQ(r.errors, 0u);
  EXPECT_EQ(r.connections_failed, 0u);
  EXPECT_EQ(r.gets + r.sets, r.ops);
  EXPECT_EQ(r.misses, 0u) << "all keys were preloaded";
  EXPECT_GT(r.get.percentile(95), 0u);
  EXPECT_GE(r.get.percentile(99), r.get.percentile(50));
  EXPECT_FALSE(r.timeline.empty());
}

TEST(LoadGenerator, UnreachableTargetIsReportedNotHung) {
  std::uint16_t port = 0;
  {
    TestServer s;
    port = s.port();
  }
  SyntheticOptions so{SyntheticOptions::Distribution::Uniform, 10, 8, 50, 0.0};
  auto w = make_synthetic_workload(so);
  const LoadResult r = run_load(test_load_opts(port), *w);
  EXPECT_EQ(r.ops, 0u);
  EXPECT_EQ(r.connections_failed, cfg_size("test.load.connections"));
}

TEST(LoadGenerator, CacheAsideRefillTurnsMissesIntoHits) {
  TestServer s;
  SyntheticOptions so{SyntheticOptions::Distribution::Uniform, cfg_size("test.load.keys"),
                      static_cast<std::uint32_t>(cfg_size("test.load.value_size")), 100, 0.0};
  auto w = make_synthetic_workload(so);  // 100% GET, nothing preloaded
  LoadOptions o = test_load_opts(s.port());
  o.warmup = Millis(0);  // scenario: measure from a cold cache, so refills are visible
  const LoadResult r = run_load(o, *w);
  EXPECT_GT(r.refills, 0u) << "misses were written back";
  EXPECT_GT(r.hits, r.misses) << "after refills, most GETs hit";
}
