# TimeSyncD: Architecture Details

This document covers the low-level systems programming architecture of the **TimeSyncD** simulation matrix.

## 1. Thread Model

The Server relies on three primary isolated Posix Threads (pthreads) to avoid IO-bound synchronization delays:
- **Acceptor Thread** (TCP Only): Runs in `select()` mode, monitoring the primary listener socket for new connections. Upon a connection, assigns the resulting FD to the active thread-safe array.
- **Broadcaster Thread**: Uses `usleep()` driven by system settings. Wakes up periodically to issue `MSG_SYNC` structs containing highly accurate monotonically-increasing nanosecond hardware times. 
- **Worker Thread**: Iterates asynchronously over all connected (TCP) and UDP channels using multiplexed non-blocking `select()` polling. Receives `MSG_REGISTER` flags mapped by clients or measures `MSG_RESPONSE` timing details.

To protect state integrity across threads, shared environments rely entirely on `pthread_mutex_t` implementations. 

## 2. IPC Data Flow

Rather than printing and logging internally (slowing down network IO components), TimeSyncD updates an IPC bridge (`shm_metrics`). 

- The `Server` calls `shm_open("timesyncd_metrics_v1")` coupled with `mmap`. 
- Every received `MSG_RESPONSE`, the Worker evaluates latency mapping and modifies fields inside this shared memory zone. 
- The external executable (`metrics_reader`) invokes identical `shm_open` paths and requests an isolated read evaluation sequence every 2 seconds.

## 3. Synchronous Timing Logic (Formulas)

### Jitter Calculation (Welford's Method)

Since calculating typical variance continuously (Jitter) is volatile and resource-intensive, TimeSyncD leverages Welford’s online algorithm. 
- The Mean (`M`) is cumulatively adjusted.
- Sums of Squares of Differences (`M2`) updates against the new Mean delta. 
- Total Variance = `M2 / (N - 1)`. 
- Typical Jitter (Standard Deviation) is ultimately `sqrt(Variance)`.

### Latency

While Network Latency often suffers asymmetric routes mapping, TimeSyncD estimates one-way Server approximations:
- `Latency = (Server Current Monotonic ns - Original Sent Monotonic ns) / 2`

### Drift Simulation (Client-Sided)

Physical clocks slowly drift dynamically. Client components inject an arbitrary static micro-skew up to 100ms when initialized (`int64_t drift_offset`). 
When a server synchronization map lands:
- `Estimated Local Monotonic = Local Hardware Time + drift_offset`
- `Drift Size = Estimated Local Monotonic - Server Monotonic`
