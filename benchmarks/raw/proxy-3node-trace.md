## Proxy + 3 nodes (2 copies per key), Twitter trace replay

- **Target:** 127.0.0.1:7100
- **Workload:** trace data/traces/cluster52-5M.csv: 5000000 requests, 932119 keys (92% GET, 6% SET, 0% DEL)
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 10000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 932119 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 x 1 | 94864 | 0.999 | 1.704 | 2.490 | 31.149 | 1.737 | 73.8 | 0 | 477513 | 186 | 250 | 156 |
| 1000 | 500 x 2 | 154784 | 6.291 | 11.141 | 20.447 | 130.716 | 12.059 | 87.9 | 0 | 282158 | 168 | 247 | 161 |
| 10000 | 500 x 20 | 544423 | 15.073 | 42.467 | 71.303 | 202.630 | 41.419 | 80.0 | 0 | 1549941 | 221 | 215 | 104 |
| 50000 | 500 x 100 | 631055 | 74.449 | 138.412 | 180.355 | 302.123 | 138.412 | 76.8 | 0 | 2250958 | 254 | 193 | 73 |

Cache-aside refills (SETs after a miss, included in throughput) per level: 100: 184329; 1000: 154934; 10000: 845520; 50000: 1114104;

**Memory:** nodes hold 609060 keys (all copies) using 191.9 MB of cache data; peak process memory (RSS) across nodes 318.7 MB, proxy 60.1 MB.
