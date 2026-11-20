## Latency target search: proxy + 3 nodes, p95 vs concurrency

- **Target:** 127.0.0.1:7100
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 71674 | 1.262 | 2.228 | 5.439 | 61.908 | 2.261 | 100.0 | 0 | 0 | 133 | 225 | 134 |
| 200 | 200 x 1 | 79422 | 2.425 | 3.932 | 7.471 | 81.146 | 3.899 | 100.0 | 0 | 0 | 135 | 237 | 149 |
| 300 | 300 x 1 | 77590 | 3.604 | 6.357 | 14.287 | 134.425 | 6.423 | 100.0 | 0 | 0 | 131 | 236 | 143 |
| 400 | 400 x 1 | 87259 | 4.391 | 7.537 | 15.204 | 73.695 | 7.406 | 100.0 | 0 | 0 | 112 | 226 | 159 |
| 500 | 500 x 1 | 93627 | 4.981 | 8.913 | 16.646 | 116.380 | 8.782 | 100.0 | 0 | 0 | 110 | 256 | 170 |
| 1000 | 500 x 2 | 136960 | 6.816 | 11.665 | 24.642 | 107.868 | 11.796 | 100.0 | 0 | 0 | 139 | 244 | 142 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 102.1 MB, proxy 8.3 MB.
