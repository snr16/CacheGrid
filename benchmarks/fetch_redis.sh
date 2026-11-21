#!/usr/bin/env bash
# Downloads and builds Redis from source for the baseline benchmark
# (config/bench/redis-*.conf). Redis needs only a C compiler and make.
#
# Version and paths come from bench.redis.* in config/bench.conf.
set -euo pipefail
cd "$(dirname "$0")/.."

conf() {
  awk -v key="$1" -F= '{ k = $1; gsub(/[ \t]+/, "", k) } k == key { sub(/^[^=]*=[ \t]*/, ""); print; exit }' config/bench.conf
}
url=$(conf bench.redis.source_url)
dir=$(conf bench.redis.build_dir)
bin=$(conf bench.redis.server_binary)

mkdir -p "$dir/src" "$(dirname "$bin")"
echo "downloading $url"
curl -fsSL "$url" | tar -xz -C "$dir/src" --strip-components 1
echo "building redis-server (a minute or two)..."
# Build inside src/: only the core server. The top-level make also builds
# optional modules (search, JSON, ...) that need extra tools and aren't used.
make -C "$dir/src/src" -j"$(getconf _NPROCESSORS_ONLN)" redis-server >"$dir/build.log" 2>&1 \
  || { tail -30 "$dir/build.log"; exit 1; }
cp "$dir/src/src/redis-server" "$bin"
echo "done: $("$bin" --version)"
