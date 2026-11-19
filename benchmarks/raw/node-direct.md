## 1 node, direct (no proxy)

- **Target:** 127.0.0.1:7001
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 133795 | 0.639 | 1.343 | 1.868 | 57.068 | 1.343 | 100.0 | 0 | 0 | 260 | - | 186 |
| 1000 | 500 x 2 | 273210 | 3.080 | 6.357 | 7.864 | 29.660 | 6.357 | 100.0 | 0 | 0 | 281 | - | 195 |
| 10000 | 500 x 20 | 1225544 | 7.209 | 18.874 | 23.331 | 52.291 | 18.874 | 100.0 | 0 | 0 | 307 | - | 157 |
| 50000 | 500 x 100 | 1688473 | 28.049 | 41.943 | 59.769 | 137.938 | 41.943 | 100.0 | 0 | 0 | 309 | - | 118 |

**Memory:** nodes hold 100000 keys (all copies) using 34.4 MB of cache data; peak process memory (RSS) across nodes 73.3 MB.
