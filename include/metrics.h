#ifndef METRICS_H
#define METRICS_H

#include "stats.h"
#include <pthread.h>
#include <stdint.h>

#define SHM_NAME "/timesyncd_metrics_v3"

/* Histogram of client-reported round-trip delays; upper bounds in microseconds, then +Inf. */
#define RTT_BUCKETS 11
extern const uint32_t rtt_bucket_us[RTT_BUCKETS - 1];

/* Telemetry block.  Lives in POSIX shared memory (read by metrics_reader) or
 * in private memory when the server runs without shm (tests). */
typedef struct {
    pthread_mutex_t mutex; /* process-shared */
    uint32_t active_clients;
    uint64_t requests;     /* requests answered */
    uint64_t expired;      /* idle clients dropped */
    uint64_t min_latency_ns, max_latency_ns;
    uint64_t rtt_hist[RTT_BUCKETS]; /* per-bucket (not cumulative) counts */
    uint64_t rtt_sum_ns, rtt_count;
    welford_t latency;     /* one-way latency estimates (client-reported RTT / 2), ns */
} metrics_shm_t;

typedef struct metrics metrics_t;

metrics_t *metrics_open(int use_shm); /* NULL on failure */
void metrics_close(metrics_t *m);
void metrics_record_request(metrics_t *m, uint64_t reported_rtt_ns);
void metrics_set_clients(metrics_t *m, uint32_t active);
void metrics_add_expired(metrics_t *m, uint64_t n);
void metrics_snapshot(metrics_t *m, metrics_shm_t *out); /* mutex field of out is meaningless */

#endif
