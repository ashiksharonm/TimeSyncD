# syntax=docker/dockerfile:1
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY common common
COPY include include
COPY server server
COPY client client
COPY ipc ipc
COPY tools tools
COPY tests tests
RUN make server_bin healthcheck OPT=-O2

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --no-create-home timesyncd
COPY --from=build /src/server_bin /usr/local/bin/timesyncd
COPY --from=build /src/healthcheck /usr/local/bin/timesyncd-healthcheck
COPY deploy/timesyncd.conf /etc/timesyncd/timesyncd.conf
USER timesyncd
EXPOSE 9000/tcp 9000/udp 9100/tcp
# Shared memory is not useful across containers; the HTTP endpoint is the interface.
CMD ["timesyncd", "--config", "/etc/timesyncd/timesyncd.conf", "--no-shm"]
HEALTHCHECK --interval=10s --timeout=3s --start-period=3s --retries=3 CMD ["timesyncd-healthcheck", "9100"]
