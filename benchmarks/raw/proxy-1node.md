## Proxy + 1 node

- **Target:** 127.0.0.1:7100
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 99450 | 0.999 | 1.327 | 1.589 | 6.915 | 1.327 | 100.0 | 0 | 0 | 101 | 267 | 177 |
| 1000 | 500 x 2 | 223299 | 4.588 | 7.143 | 8.651 | 33.549 | 7.143 | 100.0 | 0 | 0 | 82 | 288 | 192 |
| 10000 | 500 x 20 | 836313 | 11.141 | 21.758 | 33.292 | 64.247 | 21.758 | 100.0 | 0 | 0 | 157 | 256 | 141 |
| 50000 | 500 x 100 | 890256 | 31.195 | 125.829 | 150.995 | 224.930 | 125.829 | 100.0 | 0 | 0 | 170 | 233 | 114 |

**Memory:** nodes hold 100000 keys (all copies) using 34.4 MB of cache data; peak process memory (RSS) across nodes 53.0 MB, proxy 38.3 MB.
