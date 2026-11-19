# CacheGrid Architecture & Design Decisions

Living document. Each decision records **what**, **why**, and **the tradeoff**,
so it can be defended in a design review.

## Topology

```text
 Clients ──RESP/TCP──►  Proxy (stateless, 1..N)  ──RESP/TCP──►  Node 1 / Node 2 / Node 3
                         │ hash ring + vnodes                    │ sharded LRU + TTL
                         │ health checks                         │ replica store
                         └ failover routing                      └ version per key
```

## Decisions

| # | Topic | Decision | Why | Tradeoff |
|---|---|---|---|---|
| D1 | Routing | Small **proxy layer** holds the hash ring and routes requests | Clients stay simple (any RESP client, incl. `redis-cli`, works). Membership changes in one place. | Extra network hop, and the proxy can become a bottleneck or single point of failure. Mitigation: it's stateless, so several can run behind DNS or a load balancer. |
| D2 | Wire protocol | Subset of **RESP2** (Redis protocol) | Length-prefixed, so values can hold any bytes. Supports pipelining. Easy to debug with standard tools. | More parsing work than a newline-delimited text protocol. |
| D3 | Capacity unit | **Bytes** (key + value + configured per-entry overhead, 96B) | Realistic memory budget. Entry count lets a few huge values blow past it. | The overhead constant is approximate. |
| D4 | Expiration | **Lazy** on access + periodic **sweep** | Lazy is O(1) and accurate. The sweep reclaims memory from keys nobody reads again. | Sweep is O(n) today. It can switch to Redis-style random sampling if it shows up in profiles. |
| D5 | Full-cache policy | Phase 1: `noeviction` (reject). Phase 2+: **LRU** | Build in small steps that can each be tested | — |
| D6 | Concurrency | **Striped/sharded** LRU, one mutex per shard, 64 shards | Every LRU GET reorders the list, so it's a write and a reader-writer lock doesn't help. Measured: 64 shards ≈ 4× single-lock throughput on 8 threads. | LRU is per shard rather than global, so it's approximate. Max entry size = capacity / shards. |
| D7 | I/O model | N worker threads, each with its own non-blocking event loop (kqueue on macOS, epoll on Linux), all sharing the listening socket. Commands run inline on the worker. | Thousands of connections on a few threads. A cache op takes under a microsecond, so handing it to a separate pool would cost more than the op itself (same model as Memcached). | Harder to write than blocking I/O. A slow command would stall the other clients on that worker, but every command here is O(1). |
| D8 | Membership | **Static config** + health checks (no gossip/consensus) | Fits the scope. Failure detection without a consensus protocol. | Adding a node means a config change (proxy reload) |
| D9 | Replication | **Primary-backup**, RF=2. Primary writes the replica **synchronously** before acking. | A single node failure loses no acknowledged write | Write latency ≈ 2 hops |
| D10 | Conflicts | **Per-key version**, highest version wins | Simple, and works for a cache | Not linearizable during failover |
| D11 | Rejoin | A returning node starts **empty** and pulls the keys it owns from replicas before serving | It's a cache, and stale memory is worse than none | Rejoin takes time proportional to the amount of data |
| D12 | Configuration | Every tunable lives in `config/*.conf` (`key = value`). There are **no defaults in code**: a missing key is a startup error that names the key. | All tuning is visible and reviewable in one place, with no hidden magic numbers. A typo can't silently fall back to a default. | Config files must list every key. A test checks that the shipped configs stay valid. |

Phase 5 will add a detailed write-path and failover sequence for D9–D11.

## Phase 1 — Core cache

- `Cache` = `unordered_map<string, Entry{value, optional expires_at}>`.
- Time comes from an injectable `NowFn`, so tests use a fake clock and never sleep.
- `SET` without a TTL clears any previous TTL (Redis semantics).
- A failed `SET` (too large / out of memory / bad TTL) leaves the cache unchanged.
- Not thread-safe. That's added in Phase 2.

## Phase 2 — LRU eviction & thread safety

- `LRUCache` (single shard, not thread-safe) = `unordered_map<key, list::iterator>` + `std::list<Node>` ordered by recency. GET/SET/DELETE/evict are all O(1).
- `Cache` (public, thread-safe) = N × (`LRUCache` + `std::mutex`). `shard = std::hash(key) % N`. The Phase 4 hash ring must use a *different* hash function, or keys routed to one node would crowd into a few of its shards.
- `get()` returns a **copy** made while the lock is held, so callers never read memory another thread may be changing.
- `NoEviction` remains selectable via `cache.eviction_policy`.
- Verified by: LRU-order tests, a 20k-step randomized comparison against a reference model, 8-thread stress tests with invariant checks, and clean ThreadSanitizer and AddressSanitizer runs.

## Phase 3 — Network server

```text
          listening socket (shared)
         /          |           \
   Worker 0     Worker 1  ...  Worker N-1      each: Poller + its own connections
   ┌────────┐
   │ recv → in-buffer → RESP parser → CommandHandler → Cache (sharded, thread-safe)
   │                                          ↓
   │ send ← out-buffer ←──────────── RESP reply
   └────────┘
   + purge thread: cache.purge_expired() every server.purge_interval_ms
```

- **Protocol:** RESP2 arrays plus inline commands (for `nc`/telnet). Commands: `PING GET SET [EX|PX] DEL DBSIZE INFO QUIT COMMAND`.
- **Pipelining:** every complete command in the input buffer is executed before replying, and replies come back in order.
- **Defensive limits (all in config):** `max_request_bytes` and `max_args` (parser), `max_output_buffer` (disconnects a client that never reads), `max_connections` (excess clients get `-ERR max number of clients reached`).
- **Malformed input:** the client gets `-ERR Protocol error: …` and is disconnected. Other clients are unaffected.
- **Half-close:** if a client finishes sending (EOF), the server still answers the commands it received, stops watching that socket for reads (so the event loop can't spin), flushes, then closes.
- **Back-pressure:** if the kernel send buffer is full, unsent bytes stay in the connection's out-buffer and the poller reports when there's room again.
- **Latency:** `TCP_NODELAY` on every client socket, so small replies aren't held back by Nagle's algorithm.
- **Shutdown:** SIGINT/SIGTERM are handled with `sigwait` on the main thread (no async signal-handler code). Each worker is woken through its pipe, connections are closed and threads joined.
- **Open-file limit:** at startup the server raises its file-descriptor limit to `max_connections + fd_reserve`, or refuses to start. Hitting that limit at runtime would make `accept()` fail in a busy loop.
- **Portability:** the same test suite runs on macOS (kqueue, clang) and Linux (epoll, gcc) via the Dockerfile.
