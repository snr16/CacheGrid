## Proxy + 3 nodes (2 copies per key), uniform keys

- **Target:** 127.0.0.1:7100
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 85255 | 1.163 | 1.606 | 2.032 | 17.655 | 1.622 | 100.0 | 0 | 0 | 147 | 242 | 156 |
| 1000 | 500 x 2 | 132449 | 7.799 | 10.486 | 17.564 | 58.654 | 10.617 | 100.0 | 0 | 0 | 151 | 252 | 151 |
| 10000 | 500 x 20 | 729890 | 11.403 | 30.933 | 48.759 | 147.891 | 31.457 | 100.0 | 0 | 0 | 174 | 254 | 133 |
| 50000 | 500 x 100 | 914364 | 52.953 | 102.760 | 155.189 | 263.600 | 103.809 | 100.0 | 0 | 0 | 208 | 239 | 90 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 110.2 MB, proxy 59.9 MB.
