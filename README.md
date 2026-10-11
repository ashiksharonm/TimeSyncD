# TimeSyncD: Distributed Timing & Synchronization Engine

TimeSyncD is a C-based Linux/POSIX project designed to simulate distributed clock synchronization and dynamically measure critical timing characteristics—such as latency, jitter, and drift—across multiple network clients.

Inspired by real-world synchronization structures like NTP and PTP, TimeSyncD reflects core systems programming methodologies including robust multi-threading, synchronous/asynchronous POSIX networking, online statistics (Welford), and Inter-Process Communication (IPC).

## Capacity and scope

The server tracks up to 1,024 simultaneous clients (`MAX_CLIENTS` in `server/network.c`) and multiplexes them with `select()`. Precision has not been benchmarked: latency and jitter are measured, not claimed at any sub-millisecond accuracy.

## Features

- **Multi-threaded Server Architecture**: Effectively maintains decoupled accepting threads, broadcaster threads, and response aggregation workers. 
- **Protocol Flexibility**: Operates synchronously via TCP (connection-oriented) or asynchronously via UDP (connectionless).
- **Latency & Jitter Aggregation**: Computes standard performance measurements, evaluating exact network jitter using Welford's online algorithm. 
- **POSIX Shared Memory (IPC)**: Relies on `shm_open` for storing internal metrics safely evaluated by isolated reader executables.
- **Client Drift Simulation**: Each client applies a simulated random clock offset. There is no NTP-style offset correction; the server only measures round-trip latency and jitter.

## Quick Start & Build

Requires a POSIX-compliant system (ideally Linux), `gcc`, and `make`.

```bash
# 1. Build the suite
make 

# 2. Run the Server (Defaults to TCP Port 9000, 100ms interval)
./server_bin --mode tcp --port 9000 --interval 100

# 3. In another terminal, run the Reader to monitor IPC statistics
./metrics_reader

# 4. In other terminals, start clients representing workers in the synchronization context
./client_bin --mode tcp --server-ip 127.0.0.1 --port 9000
```

## Example Outputs

### Metrics Reader
![Metrics Reader Output](result/metric_reader.png)

### Server Execution
![Server Execution Output](result/server_1.png)

### Client Execution
![Client Execution Output](result/client_1.png)

## Internal Architectures

Please refer to `docs/architecture.md` for intricate specifics related to how multi-threading works, how latency and jitter are calculated, and the networking assumptions defining TCP versus UDP paradigms.
