## Failover: node2 killed and restarted under load

- **Target:** 127.0.0.1:7100
- **Workload:** uniform, 100000 keys, 256 B values, 90% GET / 10% SET
- **Client:** 2 threads, up to 500 connections; warmup 2000 ms, measured 30000 ms per level
- **On a GET miss:** the client SETs the key back (cache-aside, like a real application)
- **Preload:** 100000 keys written before measuring (0 errors)

Latencies in milliseconds. Concurrency = connections x requests in flight per connection.

CPU % = cores busy x 100 (200% = two cores), measured over the level incl. warmup.

| Concurrency | Conns x depth | Throughput (ops/s) | GET p50 | GET p95 | GET p99 | GET max | SET p95 | Hit % | Errors | Evictions | Nodes CPU % | Proxy CPU % | Client CPU % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 500 x 2 | 137782 | 7.406 | 10.093 | 11.928 | 43.931 | 10.224 | 100.0 | 0 | 0 | 128 | 261 | 160 |

**Memory:** nodes hold 200000 keys (all copies) using 68.8 MB of cache data; peak process memory (RSS) across nodes 106.2 MB, proxy 21.5 MB.

### Timeline at concurrency 1000 (every 1000 ms)

| t (s) | Throughput (ops/s) | p50 | p95 | p99 | max | Errors |
|---:|---:|---:|---:|---:|---:|---:|
| 0.0 | 140802 | 7.340 | 10.355 | 13.894 | 23.661 | 0 |
| 1.0 | 140189 | 7.274 | 11.272 | 20.447 | 28.555 | 0 |
| 2.0 | 136286 | 7.602 | 10.093 | 11.928 | 22.074 | 0 |
| 3.0 | 123906 | 8.258 | 9.961 | 10.617 | 11.480 | 0 |
| 4.0 | 129925 | 7.930 | 10.093 | 13.763 | 36.183 | 0 |
| 5.0 | 129276 | 8.126 | 10.093 | 11.010 | 13.924 | 0 |
| 6.0 | 114250 | 9.175 | 11.010 | 11.665 | 12.628 | 0 |
| 7.0 | 110787 | 9.044 | 10.879 | 11.403 | 12.233 | 0 |
| 8.0 | 143913 | 7.012 | 9.830 | 11.141 | 15.586 | 0 |
| 9.0 | 150713 | 6.750 | 8.126 | 8.913 | 14.125 | 0 |
| 10.0 | 154763 | 6.619 | 8.061 | 9.044 | 14.702 | 0 |
| 11.0 | 158774 | 6.488 | 9.175 | 14.418 | 27.390 | 0 |
| 12.0 | 167246 | 6.160 | 7.668 | 8.520 | 11.188 | 0 |
| 13.0 | 156502 | 6.619 | 8.061 | 8.651 | 11.151 | 0 |
| 14.0 | 163508 | 6.226 | 8.323 | 11.010 | 28.250 | 0 |
| 15.0 | 157608 | 6.423 | 7.864 | 9.961 | 19.779 | 0 |
| 16.0 | 145423 | 7.143 | 8.913 | 9.437 | 12.534 | 0 |
| 17.0 | 139386 | 7.406 | 9.961 | 12.714 | 19.979 | 0 |
| 18.0 | 131154 | 7.864 | 11.141 | 17.826 | 43.931 | 0 |
| 19.0 | 145587 | 6.881 | 11.403 | 18.088 | 30.628 | 0 |
| 20.0 | 118410 | 8.651 | 10.748 | 11.796 | 16.520 | 0 |
| 21.0 | 127524 | 8.061 | 10.748 | 16.777 | 31.083 | 0 |
| 22.0 | 140545 | 7.602 | 9.961 | 12.321 | 29.537 | 0 |
| 23.0 | 139499 | 7.668 | 9.961 | 10.748 | 12.679 | 0 |
| 24.0 | 134152 | 7.930 | 9.830 | 11.272 | 27.310 | 0 |
| 25.0 | 124111 | 8.389 | 10.355 | 11.534 | 27.234 | 0 |
| 26.0 | 122453 | 8.389 | 10.486 | 11.141 | 12.429 | 0 |
| 27.0 | 130793 | 7.995 | 9.961 | 10.748 | 15.899 | 0 |
| 28.0 | 133434 | 7.668 | 10.355 | 12.583 | 24.626 | 0 |
| 29.0 | 122566 | 8.389 | 10.355 | 10.879 | 13.194 | 0 |

_node2 was killed with `kill -9` at t=8s and restarted at t=16s._

Proxy log:
```
proxy: node node2@127.0.0.1:7002 is DOWN (3 failed health checks)
proxy: node node2@127.0.0.1:7002 answered again: recovering
proxy: node node2@127.0.0.1:7002 is UP (recovered)
```
