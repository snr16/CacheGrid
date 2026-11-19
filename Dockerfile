# Two-stage build:
#   1. "build": compile and run the full test suite on Linux (exercises the
#      epoll code path). The image build fails if any test fails.
#   2. runtime: a slim image with the node and proxy binaries plus configs.
#
# Normally used through docker-compose.yml (3 nodes + proxy). Standalone:
#   docker build -t cachegrid .
#   docker run --rm -p 7001:7001 cachegrid                       # one node
#   docker run --rm -p 7100:7100 cachegrid cachegrid-proxy /etc/cachegrid/docker/proxy.conf

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake --preset release \
 && cmake --build --preset release -j"$(nproc)" \
 && ctest --test-dir build/release --output-on-failure

FROM ubuntu:24.04
COPY --from=build /src/build/release/cachegrid-server /usr/local/bin/cachegrid-server
COPY --from=build /src/build/release/cachegrid-proxy /usr/local/bin/cachegrid-proxy
COPY config/ /etc/cachegrid/
USER nobody
CMD ["cachegrid-server", "/etc/cachegrid/docker/node.conf"]
