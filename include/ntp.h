#ifndef NTP_H
#define NTP_H

#include <stdint.h>

/*
 * NTP four-timestamp estimation.  t1 = client send, t2 = server receive,
 * t3 = server send, t4 = client receive.  t1/t4 are on the client clock,
 * t2/t3 on the server clock.  Offsets are (server - client), in ns.
 *
 *   offset = ((t2 - t1) + (t3 - t4)) / 2
 *   delay  = (t4 - t1) - (t3 - t2)
 *
 * The offset is exact when the path is symmetric; an asymmetry of A ns
 * between the two directions produces an offset error of A/2.
 */
int64_t ntp_offset(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);
int64_t ntp_delay(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);

/*
 * Simulated hardware clock: reads real monotonic time plus a fixed initial
 * offset plus a constant frequency error.
 */
typedef struct {
    int64_t offset_ns;
    double drift_ppm;
    uint64_t start_ns;
} simclock_t;

uint64_t simclock_read(const simclock_t *c, uint64_t real_ns);

/*
 * Clock discipline.  Each exchange yields (local_time, offset, delay) where
 * offset is measured against the *undisciplined* local clock.  The estimator
 * keeps a sliding window, drops samples whose delay is over twice the
 * window's minimum (queueing noise), and least-squares fits
 *
 *     offset(L) = alpha + beta * (L - ref)
 *
 * alpha corrects the phase and beta (a dimensionless rate, x1e6 = ppm) is the
 * frequency-error estimate.  The disciplined time is L + offset(L).
 */
#define DISC_WINDOW 32

typedef struct {
    double x[DISC_WINDOW];   /* sample time relative to ref, ns */
    double y[DISC_WINDOW];   /* measured offset, ns */
    int64_t d[DISC_WINDOW];  /* measured delay, ns */
    int n, next;
    uint64_t ref;
    int have_ref;
    double alpha, beta;
    int used; /* samples used by the last fit */
} disc_t;

void disc_init(disc_t *d);
void disc_add(disc_t *d, uint64_t local_mid_ns, int64_t offset_ns, int64_t delay_ns);
uint64_t disc_now(const disc_t *d, uint64_t local_ns);
double disc_freq_ppm(const disc_t *d);

#endif
