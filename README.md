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

## Configuration

All tunables live in `config/`. There are no defaults hidden in code, and a missing or invalid key stops startup with an error naming the key.

| File | Purpose |
|---|---|
| `config/cachegrid.conf` | Cache node settings: capacity, shard count, eviction policy |
| `config/bench.conf` | Benchmark workloads |

## Benchmarks

```bash
cmake --preset release && cmake --build --preset release -j
./build/release/shard_contention config/bench.conf
```
