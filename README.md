# CacheGrid

A distributed in-memory key-value cache in C++17, with consistent hashing, LRU eviction, replication and a RESP-compatible TCP interface.

## Build & test

```bash
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
# Sanitizers
cmake --preset asan  && cmake --build --preset asan  -j && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan  -j && ctest --preset tsan
```

Requires CMake ≥ 3.20 and a C++17 compiler. GoogleTest is downloaded automatically during the CMake configure step.

## Running a node

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
| `DEL key [key ...]` | number of keys removed |
| `DBSIZE` | number of keys |
| `INFO` | connection, hit/miss, eviction and memory stats |
| `QUIT` | `OK`, then the connection closes |

### Docker

```bash
docker build -t cachegrid .        # also runs the full test suite on Linux
docker run --rm -p 7001:7001 -v "$PWD/my-config:/etc/cachegrid:ro" cachegrid
```

The mounted `cachegrid.conf` must set `server.bind_address = 0.0.0.0` for the port to be reachable from outside the container.

## Configuration

All tunables live in `config/`. There are no defaults hidden in code, and a missing or invalid key stops startup with an error naming the key.

| File | Purpose |
|---|---|
| `config/cachegrid.conf` | Node settings: cache capacity, shards, eviction policy, network limits, threads |
| `config/bench.conf` | Benchmark workloads |

## Benchmarks

```bash
./build/release/shard_contention config/bench.conf
```

Results and methodology: `benchmarks/results.md`. Design decisions: `docs/architecture.md`.
