#ifndef STATS_H
#define STATS_H

#include <stddef.h>
#include <stdint.h>

/* Welford's online mean/variance. */
typedef struct {
    uint64_t n;
    double mean;
    double m2;
} welford_t;

void welford_add(welford_t *w, double x);
double welford_variance(const welford_t *w); /* sample variance, 0 if n < 2 */
double welford_stddev(const welford_t *w);

/* Sorts v in place; linear-interpolated percentile, p in [0,100]. 0 if n == 0. */
double stats_percentile(double *v, size_t n, double p);

#endif
