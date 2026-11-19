# Two-stage build:
#   1. "build": compile and run the full test suite on Linux (exercises the
#      epoll code path). The image build fails if any test fails.
#   2. runtime: a slim image with just the server binary and its config.
#
#   docker build -t cachegrid .
#   docker run --rm -v "$PWD/config:/etc/cachegrid:ro" -p 7001:7001 cachegrid
#
# The container reads /etc/cachegrid/cachegrid.conf. For access from outside
# the container that file must set server.bind_address = 0.0.0.0.

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
COPY config/cachegrid.conf /etc/cachegrid/cachegrid.conf
USER nobody
ENTRYPOINT ["cachegrid-server"]
CMD ["/etc/cachegrid/cachegrid.conf"]
