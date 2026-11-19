#!/usr/bin/env bash
# Downloads the start of the Twitter production cache trace used by the
# trace-replay benchmark (config/bench/proxy-3node-trace.conf).
#
# The full file is ~93 GB compressed. This streams it through zstd and keeps
# only the first bench.workload.trace_max_requests lines; once `head` has
# enough, the pipe closes and the download stops.
#
# Data: Twitter cache-trace, CC-BY 4.0 (https://github.com/twitter/cache-trace).
# Requires: curl, zstd.
set -euo pipefail
cd "$(dirname "$0")/.."

conf() {
  awk -v key="$1" -F= '{ k = $1; gsub(/[ \t]+/, "", k) } k == key { sub(/^[^=]*=[ \t]*/, ""); print; exit }' config/bench.conf
}
url=$(conf bench.trace.source_url)
out=$(conf bench.workload.trace_file)
lines=$(conf bench.workload.trace_max_requests)

mkdir -p "$(dirname "$out")"
echo "streaming the first $lines requests of $url -> $out"
# curl exits with an error when head closes the pipe early; that's expected.
(curl -sS "$url" 2>/dev/null || true) | zstd -dc 2>/dev/null | head -n "$lines" >"$out" || true
echo "done: $(wc -l <"$out") requests, $(du -h "$out" | cut -f1)"
