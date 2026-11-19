# CacheGrid

A distributed in-memory key-value cache in C++17, with consistent hashing, LRU eviction, replication and a RESP-compatible TCP interface.

```text
clients ──RESP──► cachegrid-proxy ──► node1 ┐
                  (consistent hash   ► node2 ├ cachegrid-server (sharded LRU + TTL)
                   with vnodes,      ► node3 ┘
                   2 copies per key,
                   health checks,
                   automatic recovery)
```

## Replication and failure handling

- Every key is stored on **2 nodes** (`proxy.replication_factor`): its owner on the hash ring and the next node clockwise.
- Writes go to every copy and succeed once `proxy.write_quorum` copies confirm.
- Reads use the first healthy copy and **fail over** to the other one immediately if a node doesn't answer.
- Writes carry a version number. Nodes keep only the newest version (last writer wins), and deletes leave *tombstones* so stale copies can't bring a key back.
- The proxy **health-checks** every node. A node that stops answering is marked down. When it returns, or if it restarted unnoticed, the proxy wipes it and refills it from its peers before reading from it again.
- Adding or removing a node (SIGHUP) **moves the data**: new owners are filled, old copies are trimmed, and no reads miss.

`INFO` on the proxy shows each node's state (`up` / `down` / `recovering`). Design details and the exact consistency guarantees are in `docs/architecture.md`.

## Build & test

```bash
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
# Sanitizers
cmake --preset asan  && cmake --build --preset asan  -j && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan  -j && ctest --preset tsan
```

Requires CMake ≥ 3.20 and a C++17 compiler. GoogleTest is downloaded automatically during the CMake configure step.

## Running a cluster (Docker Compose)

```bash
docker compose up --build -d     # builds the image (running all tests on Linux) and starts 3 nodes + proxy
printf 'SET user:1 alice\r\nGET user:1\r\nDBSIZE\r\nQUIT\r\n' | nc 127.0.0.1 7100
docker compose down
```

Clients connect to the proxy on **port 7100**. Nodes are also published on 7001–7003 for inspection.

**Changing membership without a restart:**
```bash
docker compose --profile scale up -d node4
# add node4@node4:7001 to proxy.nodes in config/docker/proxy.conf, then:
docker compose kill -s HUP proxy
```
Only the keys that now belong to node4 move (~1/4), and they're copied there automatically; see `benchmarks/results.md`.

**Simulating failures:**
```bash
docker compose stop node2       # reads and writes keep working (failover to the other copy)
docker compose start node2      # proxy logs: DOWN -> recovering -> UP
docker compose logs -f proxy    # watch it happen
```

## Running locally without Docker

```bash
cmake --preset release && cmake --build --preset release -j
./build/release/cachegrid-server config/cachegrid.conf      # node1 :7001
./build/release/cachegrid-server config/local/node2.conf    # node2 :7002
./build/release/cachegrid-server config/local/node3.conf    # node3 :7003
./build/release/cachegrid-proxy  config/proxy.conf          # proxy :7100
```

## Running a single node

```bash
cmake --preset release && cmake --build --preset release -j
./build/release/cachegrid-server config/cachegrid.conf
```

The server speaks the Redis protocol (RESP2), so any Redis client works, as does plain `nc`:

```bash
$ printf 'SET user:1 alice EX 60\r\nGET user:1\r\n' | nc 127.0.0.1 7001
+OK
$5
alice
```

| Command | Reply |
|---|---|
| `PING [msg]` | `PONG` or `msg` |
| `SET key value [EX seconds \| PX milliseconds]` | `OK` |
| `GET key` | value, or null if missing/expired |
| `DEL key [key ...]` | number of keys removed (via the proxy: keys may span nodes) |
| `DBSIZE` | number of keys (via the proxy: summed over all nodes) |
| `INFO` | connection, hit/miss, eviction and memory stats (proxy: routing stats and node list) |
| `QUIT` | `OK`, then the connection closes |

## Configuration

All tunables live in `config/`. There are no defaults hidden in code, and a missing or invalid key stops startup with an error naming the key.

| File | Purpose |
|---|---|
| `config/cachegrid.conf` | Node settings: cache capacity, shards, eviction policy, network limits, threads |
| `config/proxy.conf` | Proxy settings: node list, virtual nodes, timeouts, client limits |
| `config/local/*.conf` | Extra local nodes (include the base node config, override the port) |
| `config/docker/*.conf` | Container variants (include the base configs, override addresses) |
| `config/bench.conf` | Benchmark workloads |
| `config/test.conf` | Test-suite parameters |

A file can start with `include <path>` and then override individual keys.

## Benchmarks

```bash
./build/release/shard_contention config/bench.conf   # lock contention vs. shard count
./build/release/ring_analysis    config/bench.conf   # hash ring balance and key movement
```

Results and methodology: `benchmarks/results.md`. Design decisions: `docs/architecture.md`.
