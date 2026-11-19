#include "cachegrid/LoadGenerator.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "cachegrid/Config.h"
#include "cachegrid/Net.h"
#include "cachegrid/Poller.h"
#include "cachegrid/ProcessStats.h"
#include "cachegrid/Resp.h"

namespace cachegrid {

namespace {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

// Produces the op for global sequence number `seq`; false = no more ops.
using OpSource = std::function<bool(std::uint64_t seq, std::mt19937_64& rng, Op& op)>;

enum Phase : int { kWarmup = 0, kMeasure = 1, kStop = 2 };

// State shared by all client threads of one run.
struct Shared {
  std::atomic<std::uint64_t> seq{0};
  std::atomic<int> phase{kWarmup};
  std::atomic<std::int64_t> measure_start_ns{0};  // steady clock, since its epoch
};


void append_bulk(std::string& out, std::string_view s) {
  out += '$';
  out += std::to_string(s.size());
  out += "\r\n";
  out.append(s.data(), s.size());
  out += "\r\n";
}

// Writes `op` as a RESP command straight into `out` (no temporary vectors:
// this runs millions of times per second).
void encode(const Op& op, const Workload& w, const std::string& filler, std::string& out) {
  const std::string& key = w.key_name(op.key);
  switch (op.type) {
    case Op::Type::Get:
      out += "*2\r\n";
      append_bulk(out, "GET");
      append_bulk(out, key);
      break;
    case Op::Type::Del:
      out += "*2\r\n";
      append_bulk(out, "DEL");
      append_bulk(out, key);
      break;
    case Op::Type::Set:
      out += op.ttl_s ? "*5\r\n" : "*3\r\n";
      append_bulk(out, "SET");
      append_bulk(out, key);
      append_bulk(out, std::string_view(filler).substr(0, op.value_size));
      if (op.ttl_s) {
        append_bulk(out, "EX");
        append_bulk(out, std::to_string(op.ttl_s));
      }
      break;
  }
}

// Blocking connect with a timeout (setup only, not on the hot path).
int connect_with_timeout(const LoadOptions& o) {
  net::ConnectStatus st;
  const int fd = net::start_connect(o.host, o.port, st);
  if (fd < 0) return -1;
  if (st == net::ConnectStatus::InProgress) {
    pollfd p{fd, POLLOUT, 0};
    if (::poll(&p, 1, static_cast<int>(o.connect_timeout.count())) != 1 || net::socket_error(fd) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

class LoadThread {
 public:
  LoadThread(const LoadOptions& o, const Workload& w, const std::string& filler, Shared& shared, OpSource source,
             std::size_t connections, std::uint64_t seed, bool run_until_exhausted)
      : o_(o),
        w_(w),
        filler_(filler),
        shared_(shared),
        source_(std::move(source)),
        rng_(seed),
        until_exhausted_(run_until_exhausted),
        read_buf_(o.read_chunk_bytes),
        result_(o.histogram_bits),
        conns_(connections) {}

  ~LoadThread() {
    for (auto& c : conns_) {
      if (c.fd >= 0) ::close(c.fd);
    }
  }

  void run() {
    for (auto& c : conns_) {
      c.fd = connect_with_timeout(o_);
      if (c.fd < 0) {
        ++result_.connections_failed;
        c.dead = true;
        continue;
      }
      poller_.add(c.fd);
      by_fd_[c.fd] = &c;
      for (std::size_t d = 0; d < o_.depth && issue(c); ++d) {
      }
      flush(c);
    }

    std::vector<PollEvent> events;
    const int max_events = static_cast<int>(o_.events_per_poll);
    const int tick = static_cast<int>(o_.poll_tick.count());
    while (!finished()) {
      const int n = poller_.wait(events, max_events, tick);
      for (int i = 0; i < n; ++i) {
        auto it = by_fd_.find(events[static_cast<std::size_t>(i)].fd);
        if (it == by_fd_.end()) continue;
        Conn& c = *it->second;
        if (events[static_cast<std::size_t>(i)].readable) on_readable(c);
        if (!c.dead) flush(c);
      }
    }
  }

  LoadResult& result() { return result_; }

 private:
  struct Sent {
    TimePoint at;
    Op::Type type;
    bool refill;
    std::uint32_t key;  // the reply answers THIS request (the oldest in flight)
  };
  struct Conn {
    int fd = -1;
    std::string in;
    std::string out;
    std::deque<Sent> inflight;  // replies come back in send order
    std::deque<std::uint32_t> refills;  // keys to write back after a miss
    bool dead = false;
    bool exhausted = false;
    bool watch_write = false;
  };

  bool finished() const {
    if (!until_exhausted_) return shared_.phase.load(std::memory_order_relaxed) == kStop;
    for (const auto& c : conns_) {
      if (!c.dead && (!c.exhausted || !c.inflight.empty())) return false;
    }
    return true;
  }

  // Queues the next op on `c`. False when the source has no more ops.
  bool issue(Conn& c) {
    if (!c.refills.empty()) {
      const std::uint32_t key = c.refills.front();
      c.refills.pop_front();
      encode(Op{Op::Type::Set, key, w_.refill_size(key), 0}, w_, filler_, c.out);
      c.inflight.push_back(Sent{SteadyClock::now(), Op::Type::Set, true, key});
      return true;
    }
    if (c.exhausted) return false;
    Op op;
    const std::uint64_t seq = shared_.seq.fetch_add(1, std::memory_order_relaxed);
    if (!source_(seq, rng_, op)) {
      c.exhausted = true;
      return false;
    }
    encode(op, w_, filler_, c.out);
    c.inflight.push_back(Sent{SteadyClock::now(), op.type, false, op.key});
    return true;
  }

  void on_readable(Conn& c) {
    ssize_t n;
    do {
      n = ::recv(c.fd, read_buf_.data(), read_buf_.size(), 0);
    } while (n < 0 && errno == EINTR);
    if (n == 0 || (n < 0 && !net::would_block())) {
      drop(c);
      return;
    }
    if (n < 0) return;
    c.in.append(read_buf_.data(), static_cast<std::size_t>(n));

    std::size_t off = 0;
    const bool stopping = !until_exhausted_ && shared_.phase.load(std::memory_order_relaxed) == kStop;
    for (;;) {
      std::size_t len = 0;
      const auto st = resp::frame_reply(std::string_view(c.in).substr(off), o_.max_reply_bytes, len);
      if (st == resp::ParseStatus::Incomplete) break;
      if (st == resp::ParseStatus::Error || c.inflight.empty()) {
        drop(c);
        return;
      }
      on_reply(c, std::string_view(c.in).substr(off, len));
      off += len;
      if (!stopping) issue(c);
    }
    c.in.erase(0, off);
  }

  void on_reply(Conn& c, std::string_view reply) {
    const TimePoint now = SteadyClock::now();
    const Sent s = c.inflight.front();
    c.inflight.pop_front();
    const bool miss = s.type == Op::Type::Get && reply.substr(0, 3) == "$-1";
    // Cache-aside (also during warmup, so the cache reaches steady state).
    if (miss && o_.refill_on_miss && !until_exhausted_) c.refills.push_back(s.key);

    if (until_exhausted_) {
      if (reply[0] == '-') ++result_.errors;
      ++result_.ops;
      return;
    }
    // Only requests SENT inside the measured window count, so warmup
    // traffic never leaks into the numbers.
    if (shared_.phase.load(std::memory_order_relaxed) != kMeasure) return;
    const std::int64_t start_ns = shared_.measure_start_ns.load(std::memory_order_relaxed);
    const std::int64_t sent_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(s.at.time_since_epoch()).count();
    if (sent_ns < start_ns) return;

    const auto latency =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.at).count());
    const bool error = reply[0] == '-';
    ++result_.ops;
    result_.all.record(latency);
    if (error) ++result_.errors;
    switch (s.type) {
      case Op::Type::Get:
        ++result_.gets;
        result_.get.record(latency);
        if (!error) (miss ? result_.misses : result_.hits)++;
        break;
      case Op::Type::Set:
        ++result_.sets;
        if (s.refill) ++result_.refills;
        result_.set.record(latency);
        break;
      case Op::Type::Del:
        ++result_.dels;
        break;
    }

    if (o_.timeline_interval.count() > 0) {
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      const auto interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(o_.timeline_interval).count();
      const auto idx = static_cast<std::size_t>((now_ns - start_ns) / interval_ns);
      while (result_.timeline.size() <= idx) result_.timeline.emplace_back(o_.histogram_bits);
      IntervalResult& r = result_.timeline[idx];
      ++r.ops;
      if (error) ++r.errors;
      r.latency.record(latency);
    }
  }

  void flush(Conn& c) {
    while (!c.out.empty()) {
      const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), net::kSendFlags);
      if (n > 0) {
        c.out.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && net::would_block()) break;
      drop(c);
      return;
    }
    const bool want = !c.out.empty();
    if (want != c.watch_write) {
      poller_.set_interest(c.fd, true, want);
      c.watch_write = want;
    }
  }

  // Connection lost: its in-flight requests count as errors.
  void drop(Conn& c) {
    if (c.dead) return;
    ++result_.connections_failed;
    if (!until_exhausted_ && shared_.phase.load() == kMeasure) result_.errors += c.inflight.size();
    c.inflight.clear();
    poller_.remove(c.fd);
    ::close(c.fd);
    by_fd_.erase(c.fd);
    c.fd = -1;
    c.dead = true;
  }

  const LoadOptions& o_;
  const Workload& w_;
  const std::string& filler_;
  Shared& shared_;
  OpSource source_;
  std::mt19937_64 rng_;
  bool until_exhausted_;
  std::vector<char> read_buf_;
  LoadResult result_;
  Poller poller_;
  std::vector<Conn> conns_;
  std::unordered_map<int, Conn*> by_fd_;
};

std::vector<std::unique_ptr<LoadThread>> make_threads(const LoadOptions& o, const Workload& w,
                                                      const std::string& filler, Shared& shared,
                                                      const OpSource& source, bool until_exhausted) {
  std::vector<std::unique_ptr<LoadThread>> threads;
  const std::size_t n = std::max<std::size_t>(1, std::min(o.threads, o.connections));
  for (std::size_t t = 0; t < n; ++t) {
    // Spread connections as evenly as possible.
    const std::size_t conns = o.connections / n + (t < o.connections % n ? 1 : 0);
    threads.push_back(
        std::make_unique<LoadThread>(o, w, filler, shared, source, conns, o.seed + t, until_exhausted));
  }
  return threads;
}

}  // namespace

LoadResult run_load(const LoadOptions& o, const Workload& w) {
  const std::string filler(w.max_value_size(), 'x');
  Shared shared;
  const OpSource source = [&w](std::uint64_t seq, std::mt19937_64& rng, Op& op) {
    op = w.next(seq, rng);
    return true;
  };
  auto threads = make_threads(o, w, filler, shared, source, false);
  std::vector<std::thread> running;
  for (auto& t : threads) running.emplace_back([&t] { t->run(); });

  std::this_thread::sleep_for(o.warmup);
  const double cpu_before = process_cpu_seconds();
  const TimePoint start = SteadyClock::now();
  shared.measure_start_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
  shared.phase = kMeasure;
  std::this_thread::sleep_for(o.duration);
  shared.phase = kStop;
  const TimePoint end = SteadyClock::now();
  const double cpu_after = process_cpu_seconds();
  for (auto& t : running) t.join();

  LoadResult total(o.histogram_bits);
  for (auto& t : threads) {
    const LoadResult& r = t->result();
    total.all.merge(r.all);
    total.get.merge(r.get);
    total.set.merge(r.set);
    total.ops += r.ops;
    total.gets += r.gets;
    total.sets += r.sets;
    total.dels += r.dels;
    total.hits += r.hits;
    total.misses += r.misses;
    total.errors += r.errors;
    total.refills += r.refills;
    total.connections_failed += r.connections_failed;
    for (std::size_t i = 0; i < r.timeline.size(); ++i) {
      while (total.timeline.size() <= i) total.timeline.emplace_back(o.histogram_bits);
      total.timeline[i].ops += r.timeline[i].ops;
      total.timeline[i].errors += r.timeline[i].errors;
      total.timeline[i].latency.merge(r.timeline[i].latency);
    }
  }
  total.seconds = std::chrono::duration<double>(end - start).count();
  total.client_cpu_seconds = cpu_after - cpu_before;
  return total;
}

std::uint64_t run_ops_once(const LoadOptions& o, const Workload& w, const std::vector<Op>& ops) {
  const std::string filler(w.max_value_size(), 'x');
  Shared shared;
  const OpSource source = [&ops](std::uint64_t seq, std::mt19937_64&, Op& op) {
    if (seq >= ops.size()) return false;
    op = ops[seq];
    return true;
  };
  auto threads = make_threads(o, w, filler, shared, source, true);
  std::vector<std::thread> running;
  for (auto& t : threads) running.emplace_back([&t] { t->run(); });
  for (auto& t : running) t.join();
  std::uint64_t errors = 0;
  for (auto& t : threads) errors += t->result().errors + t->result().connections_failed;
  return errors;
}

LoadOptions load_options_from_config(const Config& cfg) {
  LoadOptions o{};
  o.host = cfg.get_string("bench.load.host");
  const std::size_t port = cfg.get_size("bench.load.port");
  if (port == 0 || port > 65535) throw ConfigError("'bench.load.port' must be 1..65535");
  o.port = static_cast<std::uint16_t>(port);
  o.threads = cfg.get_size("bench.load.client_threads");
  o.warmup = Millis(cfg.get_size("bench.load.warmup_ms"));
  o.duration = Millis(cfg.get_size("bench.load.duration_ms"));
  o.connect_timeout = Millis(cfg.get_size("bench.load.connect_timeout_ms"));
  o.poll_tick = Millis(cfg.get_size("bench.load.poll_tick_ms"));
  o.timeline_interval = Millis(cfg.get_size("bench.load.timeline_interval_ms"));
  o.read_chunk_bytes = cfg.get_bytes("bench.load.read_chunk_bytes");
  o.events_per_poll = cfg.get_size("bench.load.events_per_poll");
  o.max_reply_bytes = cfg.get_bytes("bench.load.max_reply_bytes");
  o.histogram_bits = static_cast<unsigned>(cfg.get_size("bench.load.histogram_precision_bits"));
  o.seed = cfg.get_size("bench.workload.seed");
  o.refill_on_miss = cfg.get_bool("bench.workload.refill_on_miss");
  const std::pair<std::size_t, const char*> positives[] = {
      {o.threads, "bench.load.client_threads"},
      {static_cast<std::size_t>(o.duration.count()), "bench.load.duration_ms"},
      {static_cast<std::size_t>(o.connect_timeout.count()), "bench.load.connect_timeout_ms"},
      {static_cast<std::size_t>(o.poll_tick.count()), "bench.load.poll_tick_ms"},
      {o.read_chunk_bytes, "bench.load.read_chunk_bytes"},
      {o.events_per_poll, "bench.load.events_per_poll"},
      {o.max_reply_bytes, "bench.load.max_reply_bytes"},
      {o.histogram_bits, "bench.load.histogram_precision_bits"},
  };
  for (const auto& [v, key] : positives) {
    if (v == 0) throw ConfigError(std::string("'") + key + "' must be >= 1");
  }
  return o;
}

}  // namespace cachegrid
