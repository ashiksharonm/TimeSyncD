# TimeSyncD: Distributed Timing & Synchronization Engine

TimeSyncD is a production-quality C-based Linux application designed to simulate distributed clock synchronization and dynamically measure critical timing characteristics—such as latency, jitter, and drift—across multiple network clients.

Inspired by real-world synchronization structures like NTP and PTP, TimeSyncD reflects core systems programming methodologies including robust multi-threading, synchronous/asynchronous POSIX networking, algorithm precision, and Inter-Process Communication (IPC).

## Features

- **Multi-threaded Server Architecture**: Effectively maintains decoupled accepting threads, broadcaster threads, and response aggregation workers. 
- **Protocol Flexibility**: Operates synchronously via TCP (connection-oriented) or asynchronously via UDP (connectionless).
- **Latency & Jitter Aggregation**: Computes standard performance measurements, evaluating exact network jitter using Welford's online algorithm. 
- **POSIX Shared Memory (IPC)**: Relies on `shm_open` for storing internal metrics safely evaluated by isolated reader executables.
- **Client Drift Simulation**: Introduces isolated time-skew to test local offset capabilities precisely.

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

### Metrics Reader:

```text
[INFO] Starting metrics reader. Monitoring /timesyncd_metrics_v1...
Clients: 1 | Avg Latency: 121us | Min: 98us | Max: 145us | Jitter: 12.43us
Clients: 3 | Avg Latency: 154us | Min: 88us | Max: 615us | Jitter: 42.10us
```

### Server Execution:
```text
[INFO] Starting TimeSyncD Server...
[INFO] Mode: TCP
[INFO] Port: 9000
[INFO] Accept thread started for TCP.
[INFO] Broadcast thread started, interval: 100 ms
[INFO] New TCP client connected from 127.0.0.1:45300.
[INFO] SIGINT received, shutting down gracefully...
```

### Client Execution:
```text
[INFO] Connected to server 127.0.0.1:9000 via TCP
[INFO] SYNC received. Latency est: 98us, Offset (Drift): 1500us
[INFO] SYNC received. Latency est: 102us, Offset (Drift): 1504us
[INFO] Client shut down.
```

## Internal Architectures

Please refer to `docs/architecture.md` for intricate specifics related to how multi-threading works, how drift and jitter are precisely calculated, and the networking assumptions defining TCP versus UDP paradigms.
