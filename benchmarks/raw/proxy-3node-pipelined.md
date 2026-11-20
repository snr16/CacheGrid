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
| 100 | 100 x 1 | 98379 | 0.942 | 1.688 | 2.884 | 44.740 | 1.704 | 100.0 | 0 | 0 | 122 | 233 | 151 |
| 200 | 100 x 2 | 107816 | 1.819 | 2.654 | 5.439 | 43.516 | 2.687 | 100.0 | 0 | 0 | 146 | 230 | 129 |
| 300 | 100 x 3 | 136706 | 2.163 | 3.244 | 6.947 | 67.050 | 3.277 | 100.0 | 0 | 0 | 148 | 226 | 129 |
| 400 | 100 x 4 | 161857 | 2.327 | 3.867 | 8.782 | 129.299 | 3.899 | 100.0 | 0 | 0 | 151 | 225 | 122 |
| 600 | 100 x 6 | 221152 | 2.195 | 5.243 | 13.631 | 84.332 | 5.308 | 100.0 | 0 | 0 | 142 | 205 | 114 |
| 800 | 100 x 8 | 291729 | 2.163 | 5.702 | 14.811 | 161.585 | 5.833 | 100.0 | 0 | 0 | 147 | 212 | 116 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 100.8 MB, proxy 5.7 MB.
