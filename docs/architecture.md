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
| D1 | Routing | Small **proxy layer** holds the hash ring and routes requests. It's fully asynchronous: one pipelined connection per node per worker, plus per-client ordered reply slots. | Clients stay simple (any RESP client works). Membership changes in one place. | Extra network hop, and the proxy can become a bottleneck or single point of failure. Mitigation: it's stateless, so several can run behind DNS or a load balancer. |
| D2 | Wire protocol | Subset of **RESP2** (Redis protocol) | Length-prefixed, so values can hold any bytes. Supports pipelining. Easy to debug with standard tools. | More parsing work than a newline-delimited text protocol. |
| D3 | Capacity unit | **Bytes** (key + value + configured per-entry overhead, 96B) | Realistic memory budget. Entry count lets a few huge values blow past it. | The overhead constant is approximate. |
| D4 | Expiration | **Lazy** on access + periodic **sweep** | Lazy is O(1) and accurate. The sweep reclaims memory from keys nobody reads again. | Sweep is O(n) today. It can switch to Redis-style random sampling if it shows up in profiles. |
| D5 | Full-cache policy | Phase 1: `noeviction` (reject). Phase 2+: **LRU** | Build in small steps that can each be tested | — |
| D6 | Concurrency | **Striped/sharded** LRU, one mutex per shard, 64 shards | Every LRU GET reorders the list, so it's a write and a reader-writer lock doesn't help. Measured: 64 shards ≈ 4× single-lock throughput on 8 threads. | LRU is per shard rather than global, so it's approximate. Max entry size = capacity / shards. |
| D7 | I/O model | N worker threads, each with its own non-blocking event loop (kqueue on macOS, epoll on Linux), all sharing the listening socket. Commands run inline on the worker. | Thousands of connections on a few threads. A cache op takes under a microsecond, so handing it to a separate pool would cost more than the op itself (same model as Memcached). | Harder to write than blocking I/O. A slow command would stall the other clients on that worker, but every command here is O(1). |
| D8 | Membership | **Static config** (`proxy.nodes`), reloaded live on **SIGHUP**. **Health checks** by a proxy background thread: `RUNID` every `health_interval_ms`; `health_fail_threshold` failures in a row mark a node Down; a changed run id means it restarted. No gossip or consensus. | Fits the scope. Detects both slow failures (timeouts) and fast crash-restarts (run id). | Each proxy judges health on its own; two proxies may briefly disagree. |
| D9 | Replication | **RF copies** (default 2): the ring owner plus the next distinct nodes clockwise. The **proxy** writes all copies in parallel and replies OK once **`write_quorum`** confirm (default 1). Reads go to the first healthy copy and **fail over** to the next on error. | Nodes stay simple, since replication reuses the proxy's fan-out. W=1 keeps writes available with a copy down. | W=1 → a write acknowledged by one copy is lost if that node dies before its peer is refilled. W=RF trades availability for durability. Not atomic: a write that misses quorum may still be applied on some copies. |
| D10 | Conflicts | **Last writer wins by version.** Version = proxy wall clock in µs << 8 \| `proxy.id`, never decreasing. Nodes apply `VSET`/`VDEL` only if the version is newer. Deletes leave **tombstones** (`cache.tombstone_ttl_ms`). | Order-independent: replays, retries and recovery copies can't overwrite newer data, and deletes can't be undone by stale copies. | With several proxies, clock skew decides near-simultaneous writes. Not linearizable. A tombstone evicted by LRU stops protecting its key. |
| D11 | Rejoin / rebalance | **Recovery:** FLUSHALL the returning node (it may hold stale data), mark it *Recovering* (gets writes, not reads), wait `recovery_grace_ms` (≥ request timeout, so in-flight writes land), copy every key it owns from its peers via `SCAN` (versions and TTLs kept), then mark it Up. **Membership change:** fill every node with its keys, *then* trim keys it no longer owns. | A wiped-and-refilled node can never serve outdated values. Fill-before-trim never deletes a last copy. | Recovery time is proportional to data size. Between a fast restart and its detection (< one health interval), reads may miss on the restarted node. |
| D13 | Ring hash | FNV-1a + Murmur3 finalizer, 160 virtual nodes per node, hashing the node **id** (not its address) | Deterministic on every platform (unlike `std::hash`), and different from the per-node shard hash. Ids keep keys in place if a node's address changes. | Not cryptographic, which doesn't matter for placement |
| D14 | Proxy hot path | No per-request heap allocation besides the request buffer (precomputed node connections, index-based owner lookup, reused scratch buffers) | Profiling showed malloc/free plus allocator lock contention rivalling socket I/O | Slightly more bookkeeping when membership changes |
| D12 | Configuration | Every tunable lives in `config/*.conf` (`key = value`), test parameters included (`config/test.conf`). There are **no defaults in code**: a missing key is a startup error that names the key. Files can `include` a base file and override keys. | All tuning is visible and reviewable in one place, with no hidden magic numbers. A typo can't silently fall back to a default. | Config files must list every key. A test checks that the shipped configs stay valid. |

See the Phase 5 section for the write path, failover and recovery sequence.

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

## Phase 4 — Proxy & consistent hashing

```text
 client ─► proxy worker ─┬─► node1   one pipelined connection per node per worker
                          ├─► node2   (replies arrive in order per connection)
                          └─► node3
           per client: [slot1][slot2][slot3]...   replies released strictly in order
```

- **Routing:** `owner = ring.owner(key)`. The ring has 160 points per node, sorted. Lookup is a binary search, O(log V).
- **Commands:** GET/SET go to the owner. Multi-key DEL is split per owner, sent in parallel and the counts summed. DBSIZE goes to every node and is summed. PING/INFO/QUIT/COMMAND are answered by the proxy itself.
- **Ordering:** every command takes a numbered slot in its client's queue. A node's reply fills its slot, and only filled slots at the front are sent. Pipelined replies therefore stay in order even when nodes answer out of order.
- **Back-pressure:** a client may have `proxy.max_pending_per_client` commands in flight. Beyond that the proxy stops reading from it until replies drain.
- **Node failure:** connection refused/reset, `connect_timeout_ms` or `request_timeout_ms` fails every request waiting on that node with `-ERR node <id> (...) unavailable`. For `reconnect_backoff_ms` afterwards, requests to that node fail immediately instead of waiting. Other nodes are unaffected.
- **Membership reload:** SIGHUP re-reads `proxy.nodes`. Workers pick up the new ring through a version counter, so there's no lock per request.
- **Data migration (Phase 5):** after a membership change, the health thread copies moved keys to their new owners and trims old copies.

**Resolved in Phase 5:** stale copies left on a key's old owner are now trimmed after every membership change, and new owners are filled from existing copies (no misses).

**Measured** (`benchmarks/results.md`): adding a 4th node moved 23% of keys, all to the new node. `hash % N` would move 75%. Live cluster: 698 of 3,000 keys (23.3%) moved after adding node4.

## Phase 5 — Replication & failure recovery

```text
SET k v  ─► proxy: version = next_version()
            ├─► VSET <ver> k v ─► owner     ─┐  OK once write_quorum
            └─► VSET <ver> k v ─► replica   ─┘  copies confirm
GET k    ─► first Up copy ── fails? ──► next copy (failover, no waiting for health checks)

health thread:  RUNID each node ─ fails×N ─► Down ─ answers again ─► FLUSHALL ─► Recovering
                                  run id changed ─────────────────┘   grace ─► fill from peers ─► Up
```

**Node states:** *Up* (reads + writes), *Down* (skipped for writes, last resort for reads), *Recovering* (writes only).

**Internal node commands:** `VSET version key value [EX|PX n]` (+OK / +STALE), `VDEL version key`, `SCAN shard` (entries with version, TTL and tombstone flag), `FLUSHALL`, `RUNID`.

**Consistency guarantees** (W=1, RF=2):
- A read never returns a value *older than* the newest one on its copy; a recovered node never serves pre-outage data.
- A deleted key never reappears through recovery or rebalancing (tombstones), within `tombstone_ttl_ms`.
- Copies converge: after recovery/rebalance every owner holds the newest version of every key.
- **Not guaranteed:** read-your-writes across a failover (the replica might not have a write that only reached the owner before it died), linearizability, or ordering across proxies beyond their clock accuracy.

**DBSIZE through the proxy** counts stored copies (keys × RF).

**Measured on the live cluster:** see `benchmarks/results.md`. With node2 down: 3,000/3,000 reads and writes succeeded (Phase 4: 1,056 errors). After node2 recovered and node1 was stopped as well: 3,000/3,000 reads returned the newest value. Adding node4: 0 misses (Phase 4: 698), 6,000 copies (no stale copies left).

## Phase 6 — Benchmarking & optimization

**Load tester** (`cachegrid-bench`, engine in `src/LoadGenerator.cpp`):
- N client threads, each with its own event loop over its share of connections. Every connection keeps exactly `depth` requests in flight (closed loop); **concurrency = connections × depth**.
- Per-request latency = reply time − send time. Send times are queued per connection, since replies arrive in order. Latencies go into a log-linear histogram (`LatencyHistogram`, ~1.6% relative precision, O(1) record, merged across threads).
- Only requests *sent* inside the measured window are counted, so warmup never leaks into results.
- Workloads (`src/Workload.cpp`): uniform, Zipfian (YCSB/Gray algorithm), and trace replay (Twitter cache-trace CSV: get/gets → GET; set/add/replace/cas/append/prepend/incr/decr → SET at the recorded size and TTL; delete → DEL). Optional cache-aside refill (SET after a GET miss).
- Server CPU and memory are read from each server's `INFO` (`used_cpu_seconds`, `peak_rss_bytes`, from `getrusage`) before and after every level.

**Optimization (D14):** profiling showed the proxy spending about as much time in `malloc`/`free` (and waiting on the allocator's lock) as on socket I/O. The per-request path is now allocation-free apart from the request buffer: node connections are resolved once per membership change (`node_backend_`), owners come from `HashRing::owner_positions` into reused buffers, commands are compared case-insensitively without copying, and VSET/VDEL are encoded in place. Measured: +49–57% throughput and −21–28% p95 at 10k–50k concurrency (`benchmarks/results.md`).

**Results summary:** p95 < 5 ms holds up to ~300 concurrent requests through the proxy (1.6 ms at 100); peak ~0.9–1.0M ops/s through the proxy and 1.7M ops/s on a node directly, on one 8-core laptop; 0 errors while a node is killed under load.
