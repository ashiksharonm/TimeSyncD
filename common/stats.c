#include "../include/stats.h"
#include <math.h>
#include <stdlib.h>

void welford_add(welford_t *w, double x) {
    w->n++;
    double delta = x - w->mean;
    w->mean += delta / (double)w->n;
    w->m2 += delta * (x - w->mean);
}

double welford_variance(const welford_t *w) {
    return w->n > 1 ? w->m2 / (double)(w->n - 1) : 0.0;
}

double welford_stddev(const welford_t *w) {
    return sqrt(welford_variance(w));
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double stats_percentile(double *v, size_t n, double p) {
    if (n == 0)
        return 0.0;
    qsort(v, n, sizeof(double), cmp_double);
    double rank = p / 100.0 * (double)(n - 1);
    size_t lo = (size_t)rank;
    size_t hi = lo + 1 < n ? lo + 1 : lo;
    return v[lo] + (v[hi] - v[lo]) * (rank - (double)lo);
}
