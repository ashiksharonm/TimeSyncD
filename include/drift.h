#ifndef DRIFT_H
#define DRIFT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Per-client clock-drift tracking, from the server's side only.
 *
 * Every request carries the client's clock reading t1 and the client's previous
 * round-trip delay.  The server compares t1 with its own receive time t2:
 *
 *     offset_sample = (t2 - t1) - last_delay / 2
 *
 * (the usual symmetric-path assumption).  A client clock that runs fast by d ppm
 * makes this offset shrink by d microseconds per second, so a recency-weighted
 * least-squares fit of offset against t1 yields the slope, and
 *
 *     drift_ppm = -slope * 1e6        (positive = the client's clock is fast)
 *
 * Tracking is keyed by client_id, kept in an open-addressing table with a hard
 * capacity, and entries not seen recently are expired.
 */
typedef struct drift_table drift_table_t;

typedef struct {
    uint32_t client_id;
    uint32_t samples;
    double drift_ppm;   /* valid only if ready */
    double offset_us;   /* latest fitted offset (server - client) */
    double span_s;      /* client-clock time covered by the samples */
    int ready;          /* >= DRIFT_MIN_SAMPLES samples spanning >= DRIFT_MIN_SPAN_S */
} drift_entry_t;

#define DRIFT_MIN_SAMPLES 8
#define DRIFT_MIN_SPAN_S 2.0

drift_table_t *drift_create(size_t max_clients);
void drift_destroy(drift_table_t *t);
/* Returns 0, or -1 if the table is full and the client is new (counted in drift_dropped). */
int drift_observe(drift_table_t *t, uint32_t client_id, uint64_t t1, uint64_t t2,
                  uint64_t last_delay_ns, uint64_t now_ns);
size_t drift_expire(drift_table_t *t, uint64_t seen_before_ns);
size_t drift_count(const drift_table_t *t);
uint64_t drift_dropped(const drift_table_t *t);
/* Copies up to max ready entries, those with the largest |drift| first. Returns the number copied. */
size_t drift_top(const drift_table_t *t, drift_entry_t *out, size_t max);
/* Summary over all ready entries: count and |drift_ppm| percentiles (p50, p99) and max. */
typedef struct { size_t ready; double abs_p50, abs_p99, abs_max; } drift_summary_t;
void drift_summary(const drift_table_t *t, drift_summary_t *out);

#endif
