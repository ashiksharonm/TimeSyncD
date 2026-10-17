#ifndef EXPOSITION_H
#define EXPOSITION_H

#include "drift.h"
#include "metrics.h"
#include "server.h"
#include <stddef.h>

/* Everything the metrics endpoint renders, gathered by the server. */
typedef struct {
    server_stats_t srv;
    metrics_shm_t m;
    drift_summary_t drift;
    size_t drift_tracked;
    uint64_t drift_dropped;
    const char *backend;
    int tcp;
    double uptime_s;
    const drift_entry_t *top;
    size_t ntop;
} expo_t;

/* Both return the number of bytes written (always NUL-terminated, truncated at cap-1). */
size_t expo_prometheus(char *buf, size_t cap, const expo_t *e);
size_t expo_json(char *buf, size_t cap, const expo_t *e);

#endif
