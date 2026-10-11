#include "../include/ntp.h"
#include <string.h>

int64_t ntp_offset(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    return ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
}

int64_t ntp_delay(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    return (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
}

uint64_t simclock_read(const simclock_t *c, uint64_t real_ns) {
    double skew = (double)(int64_t)(real_ns - c->start_ns) * c->drift_ppm * 1e-6;
    return real_ns + (uint64_t)(c->offset_ns + (int64_t)skew);
}

void disc_init(disc_t *d) {
    memset(d, 0, sizeof(*d));
}

static void disc_fit(disc_t *d) {
    int64_t mind = INT64_MAX;
    for (int i = 0; i < d->n; i++)
        if (d->d[i] < mind)
            mind = d->d[i];

    int m = 0, last = -1;
    double sx = 0, sy = 0;
    /* ring order is irrelevant for OLS; "latest" is the slot before next */
    int latest = (d->next + DISC_WINDOW - 1) % DISC_WINDOW;
    for (int i = 0; i < d->n; i++) {
        if (d->d[i] > 2 * mind)
            continue;
        m++;
        sx += d->x[i];
        sy += d->y[i];
        last = i;
    }
    d->used = m;
    if (m == 0)
        return;
    if (d->d[latest] <= 2 * mind)
        last = latest;
    if (m < 3) { /* not enough points for a slope: track phase only */
        d->alpha = d->y[last];
        d->beta = 0.0;
        return;
    }
    double mx = sx / m, my = sy / m, sxx = 0, sxy = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->d[i] > 2 * mind)
            continue;
        sxx += (d->x[i] - mx) * (d->x[i] - mx);
        sxy += (d->x[i] - mx) * (d->y[i] - my);
    }
    double beta = sxx > 0 ? sxy / sxx : 0.0;
    if (beta > 1e-3) beta = 1e-3;     /* clamp to +-1000 ppm */
    if (beta < -1e-3) beta = -1e-3;
    d->beta = beta;
    d->alpha = my - beta * mx;
}

void disc_add(disc_t *d, uint64_t local_mid_ns, int64_t offset_ns, int64_t delay_ns) {
    if (!d->have_ref) {
        d->ref = local_mid_ns;
        d->have_ref = 1;
    }
    d->x[d->next] = (double)(int64_t)(local_mid_ns - d->ref);
    d->y[d->next] = (double)offset_ns;
    d->d[d->next] = delay_ns;
    d->next = (d->next + 1) % DISC_WINDOW;
    if (d->n < DISC_WINDOW)
        d->n++;
    disc_fit(d);
}

uint64_t disc_now(const disc_t *d, uint64_t local_ns) {
    if (!d->have_ref)
        return local_ns;
    double x = (double)(int64_t)(local_ns - d->ref);
    return local_ns + (uint64_t)(int64_t)(d->alpha + d->beta * x);
}

double disc_freq_ppm(const disc_t *d) {
    return d->beta * 1e6;
}
