# TimeSyncD architecture

## Process model

`server_bin` is a single-threaded `poll()` event loop (`server/server_core.c`). One pollfd array holds the listening socket (or the UDP socket) followed by one entry per TCP connection; the table grows on demand, so the only limits are file descriptors and memory (the old `select()` design capped out at `FD_SETSIZE`/1024).

- TCP: each connection owns a `frame_buf_t`. A readable event does one `recv()` into it and every complete frame is answered; a partial frame simply waits for the next event. Replies that the socket will not take immediately go into a small per-connection output queue flushed on `POLLOUT`; a client whose queue overflows is disconnected. `accept()` failures with `EMFILE`/`ENFILE` pause polling of the listener for 100 ms instead of spinning on the still-pending connection.
- UDP: one socket; each datagram holds exactly one frame. Peers are tracked in an open-addressing hash table keyed by (address, port) so they can be counted and expired.
- Expiry: once per second, TCP connections and UDP peers silent for longer than `--idle-sec` (default 30) are dropped.
- No lock is held across I/O. The only mutex protects the telemetry block, for a few counter updates.

## Protocol (`include/protocol.h`)

Length-prefixed frames, all integers big-endian: `u32 payload_len | type, version, reserved | client_id | seq | t1 | t2 | t3 | last_delay_ns` (44-byte payload). Encoding and decoding go through explicit byte-wise accessors, never struct casts. Payloads of 44-64 bytes are accepted and trailing bytes ignored; any other length is a protocol error and closes the TCP connection.

## Time synchronisation

Client-driven NTP-style exchange: the client stamps `t1` (its own raw hardware clock) on a request, the server adds `t2` (receive) and `t3` (send), the client reads `t4` on arrival.

    offset = ((t2 - t1) + (t3 - t4)) / 2        delay = (t4 - t1) - (t3 - t2)

Offset is exact on a symmetric path; a path asymmetry of A produces an offset error of A/2.

### Client clock simulation and discipline (`common/ntp.c`, `client/client_core.c`)

Each client has a simulated oscillator: `local = real + offset0 + drift_ppm * 1e-6 * (real - start)`, with offset0 and drift drawn at random from configurable maxima. The discipline loop keeps a sliding window of (time, offset, delay) samples, discards samples whose delay exceeds twice the window minimum, and least-squares fits `offset(L) = alpha + beta * (L - ref)`. `alpha` corrects the phase, `beta` is the frequency-error estimate (reported in ppm). The disciplined clock is `L + alpha + beta * (L - ref)`. On localhost the server's clock is the same `CLOCK_MONOTONIC`, so `disciplined - real` is the true residual error and is what the client logs and the load test measures.

## Telemetry (`server/metrics.c`)

A process-shared-mutex block in POSIX shared memory (`/timesyncd_metrics_v2`): active clients, requests, expired clients, and min/max/mean/jitter of the one-way latency (clients report their previous round-trip delay in each request; the server feeds `delay/2` into Welford's online algorithm). `metrics_reader` prints it every 2 seconds. Tests run the server with private (non-shm) telemetry.

## Tests

- `tests/test_unit.c`: Welford (against a two-pass reference, including a huge-offset series), percentiles, exact wire bytes, framing (byte-at-a-time, multiple frames, oversize/undersize prefixes, longer payloads), NTP offset/delay with asymmetry, convergence of the discipline loop on synthetic jittery exchanges.
- `tests/test_integration.c`: real server thread + clients over loopback (TCP and UDP convergence and telemetry; byte-at-a-time requests on the wire; garbage prefix drops only that connection; idle expiry; 1500 concurrent TCP clients).
