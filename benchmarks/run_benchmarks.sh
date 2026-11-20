#!/usr/bin/env bash
# Runs the load-test scenarios against a local cluster of native processes
# (no Docker, so no virtual-network overhead) and saves each report to
# benchmarks/raw/<scenario>.md. Every setting comes from config/.
#
#   benchmarks/run_benchmarks.sh               # all scenarios
#   benchmarks/run_benchmarks.sh failover      # only the named one(s)
#
# Scenarios = files in config/bench/ (without .conf).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=build/release
RAW=benchmarks/raw
LOGS="$RAW/logs"
mkdir -p "$RAW" "$LOGS"

# Reads one key from a config file (for the few settings this script needs).
conf() {
  awk -v key="$1" -F= '{ k = $1; gsub(/[ \t]+/, "", k) } k == key { sub(/^[^=]*=[ \t]*/, ""); print; exit }' "$2"
}

echo "building release binaries..."
cmake --preset release >/dev/null
cmake --build --preset release -j >/dev/null

PIDS=()
LAST_PID=
# Log line that means "ready" (CacheGrid's; Redis prints a different one).
READY="listening on"
# start <name> <command...>: run in the background, wait until it's listening.
start() {
  local log="$LOGS/$1.log"
  shift
  "$@" >"$log" 2>&1 &
  LAST_PID=$!
  PIDS+=("$LAST_PID")
  for _ in $(seq 1 100); do
    grep -q "$READY" "$log" && return 0
    sleep 0.1
  done
  echo "failed to start: $*" >&2
  cat "$log" >&2
  exit 1
}

stop_all() {
  if [ ${#PIDS[@]} -gt 0 ]; then
    kill -TERM "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
  fi
  PIDS=()
}
trap stop_all EXIT

NODE2_PID=
nodes() {  # nodes <count>
  start node1 "$BIN/cachegrid-server" config/cachegrid.conf
  if [ "$1" -ge 2 ]; then
    start node2 "$BIN/cachegrid-server" config/local/node2.conf
    NODE2_PID=$LAST_PID
  fi
  if [ "$1" -ge 3 ]; then start node3 "$BIN/cachegrid-server" config/local/node3.conf; fi
}

# Real Redis as a baseline: redis <server config>
redis() {
  local bin
  bin=$(conf bench.redis.server_binary config/bench.conf)
  if [ ! -x "$bin" ]; then
    echo "   skipped: $bin not found (run benchmarks/fetch_redis.sh first)"
    return 1
  fi
  READY="Ready to accept connections" start redis "$bin" "$1"
}

bench() {  # bench <scenario>
  "$BIN/cachegrid-bench" "config/bench/$1.conf" | tee "$RAW/$1.md"
}

# Steady load while node2 crashes (kill -9) and comes back.
failover() {
  local warmup_ms kill_after restart_after
  warmup_ms=$(conf bench.load.warmup_ms config/bench.conf)
  kill_after=$(conf bench.failover.kill_after_s config/bench.conf)
  restart_after=$(conf bench.failover.restart_after_s config/bench.conf)

  "$BIN/cachegrid-bench" config/bench/failover.conf >"$RAW/failover.md" &
  local bench_pid=$!
  # The report's table header appears once preloading is done.
  until grep -q "Latencies in milliseconds" "$RAW/failover.md" 2>/dev/null; do sleep 0.1; done
  sleep "$(awk -v ms="$warmup_ms" 'BEGIN { print ms / 1000 }')"
  sleep "$kill_after"
  echo "   killing node2 (kill -9) at t=${kill_after}s"
  kill -9 "$NODE2_PID"
  sleep "$restart_after"
  echo "   restarting node2 at t=$((kill_after + restart_after))s"
  start node2-restarted "$BIN/cachegrid-server" config/local/node2.conf
  wait "$bench_pid"
  {
    echo
    echo "_node2 was killed with \`kill -9\` at t=${kill_after}s and restarted at t=$((kill_after + restart_after))s._"
    echo
    echo "Proxy log:"
    echo '```'
    grep "proxy:" "$LOGS/proxy.log" || true
    echo '```'
  } >>"$RAW/failover.md"
  cat "$RAW/failover.md"
}

run() {
  echo
  echo ">> scenario: $1"
  case "$1" in
    node-direct) nodes 1; bench node-direct ;;
    proxy-1node)
      nodes 1
      start proxy "$BIN/cachegrid-proxy" config/local/proxy-1node.conf
      bench proxy-1node
      ;;
    proxy-3node | proxy-3node-zipf | proxy-3node-trace | proxy-3node-slo | proxy-3node-pipelined)
      nodes 3
      start proxy "$BIN/cachegrid-proxy" config/proxy.conf
      bench "$1"
      ;;
    redis-direct) if redis config/redis/redis.conf; then bench redis-direct; fi ;;
    redis-io-threads) if redis config/redis/redis-io-threads.conf; then bench redis-io-threads; fi ;;
    failover)
      nodes 3
      start proxy "$BIN/cachegrid-proxy" config/proxy.conf
      failover
      ;;
    *) echo "unknown scenario '$1' (see config/bench/)" >&2; exit 2 ;;
  esac
  stop_all
}

if [ $# -gt 0 ]; then
  for s in "$@"; do run "$s"; done
else
  for s in node-direct redis-direct redis-io-threads proxy-1node proxy-3node proxy-3node-slo proxy-3node-pipelined \
    proxy-3node-zipf \
    proxy-3node-trace failover; do run "$s"; done
fi
