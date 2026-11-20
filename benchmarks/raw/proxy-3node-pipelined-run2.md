## Pipelined clients: proxy + 3 nodes, 100 connections x depth

- **Target:** 127.0.0.1:7100
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 100 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 101528 | 0.942 | 1.507 | 2.392 | 104.464 | 1.540 | 100.0 | 0 | 0 | 133 | 258 | 158 |
| 200 | 100 x 2 | 102478 | 1.835 | 2.851 | 7.209 | 97.581 | 2.884 | 100.0 | 0 | 0 | 140 | 218 | 122 |
| 300 | 100 x 3 | 166650 | 1.753 | 2.949 | 5.702 | 115.302 | 2.982 | 100.0 | 0 | 0 | 145 | 228 | 137 |
| 400 | 100 x 4 | 165747 | 2.294 | 3.736 | 8.061 | 54.786 | 3.801 | 100.0 | 0 | 0 | 151 | 219 | 124 |
| 600 | 100 x 6 | 218752 | 2.523 | 4.850 | 7.471 | 39.641 | 4.850 | 100.0 | 0 | 0 | 160 | 232 | 128 |
| 800 | 100 x 8 | 248099 | 2.097 | 7.340 | 24.379 | 304.010 | 7.471 | 100.0 | 0 | 0 | 138 | 191 | 96 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 102.4 MB, proxy 6.3 MB.
