## Proxy + 3 nodes (2 copies per key), Zipfian keys

- **Target:** 127.0.0.1:7100
- **Workload:** zipf(theta=0.99), 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 84386 | 1.180 | 1.786 | 2.392 | 27.003 | 1.769 | 100.0 | 0 | 0 | 144 | 251 | 155 |
| 1000 | 500 x 2 | 135424 | 7.668 | 10.617 | 17.039 | 61.642 | 10.617 | 100.0 | 0 | 0 | 149 | 251 | 150 |
| 10000 | 500 x 20 | 747163 | 10.748 | 30.933 | 50.856 | 115.980 | 31.195 | 100.0 | 0 | 0 | 151 | 264 | 132 |
| 50000 | 500 x 100 | 975798 | 46.662 | 94.372 | 146.801 | 255.590 | 94.372 | 100.0 | 0 | 0 | 178 | 249 | 94 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 105.2 MB, proxy 59.4 MB.
