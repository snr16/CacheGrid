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
| 100 | 100 x 1 | 78324 | 1.229 | 1.802 | 2.884 | 72.680 | 1.802 | 100.0 | 0 | 0 | 136 | 227 | 146 |
| 200 | 200 x 1 | 82142 | 2.425 | 3.342 | 4.588 | 30.185 | 3.342 | 100.0 | 0 | 0 | 148 | 254 | 152 |
| 300 | 300 x 1 | 83906 | 3.506 | 4.653 | 9.306 | 69.255 | 4.653 | 100.0 | 0 | 0 | 144 | 244 | 154 |
| 400 | 400 x 1 | 83959 | 4.784 | 6.226 | 9.830 | 60.328 | 6.226 | 100.0 | 0 | 0 | 147 | 256 | 154 |
| 500 | 500 x 1 | 102929 | 4.391 | 7.995 | 10.224 | 41.821 | 7.733 | 100.0 | 0 | 0 | 117 | 279 | 186 |
| 1000 | 500 x 2 | 127758 | 8.126 | 10.748 | 12.452 | 38.053 | 10.748 | 100.0 | 0 | 0 | 160 | 259 | 146 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 104.9 MB, proxy 7.8 MB.
