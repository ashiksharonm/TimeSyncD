#include "../include/drift.h"
#include "../include/stats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FORGET 0.98 /* per-sample weight decay: the fit follows the last ~50 samples */

typedef struct {
    uint32_t id;
    uint32_t samples;
    int used;
    uint64_t last_seen_ns;
    uint64_t x0_ns;      /* t1 of the first sample */
    int64_t y0_ns;       /* offset of the first sample (the fit is relative to it) */
    double w, mx, my, sxx, sxy;
    double last_x;
} slot_t;

struct drift_table {
    slot_t *slots;
    size_t cap, count, max_clients;
    uint64_t dropped;
};

drift_table_t *drift_create(size_t max_clients) {
    drift_table_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    size_t cap = 64;
    while (cap < max_clients * 2) cap *= 2;
    t->slots = calloc(cap, sizeof(slot_t));
    if (!t->slots) { free(t); return NULL; }
    t->cap = cap;
    t->max_clients = max_clients;
    return t;
}

void drift_destroy(drift_table_t *t) {
    if (!t) return;
    free(t->slots);
    free(t);
}

static size_t hash_id(uint32_t id, size_t mask) {
    uint64_t h = (uint64_t)id * 0x9E3779B97F4A7C15ULL;
    return (size_t)(h >> 32) & mask;
}

static slot_t *find(drift_table_t *t, uint32_t id, int create) {
    size_t i = hash_id(id, t->cap - 1);
    for (size_t n = 0; n < t->cap; n++, i = (i + 1) & (t->cap - 1)) {
        slot_t *s = &t->slots[i];
        if (!s->used) return create ? s : NULL;
        if (s->id == id) return s;
    }
    return NULL;
}

int drift_observe(drift_table_t *t, uint32_t id, uint64_t t1, uint64_t t2, uint64_t last_delay_ns,
                  uint64_t now_ns) {
    slot_t *s = find(t, id, 0);
    if (!s) {
        if (t->count >= t->max_clients) { t->dropped++; return -1; }
        s = find(t, id, 1);
        if (!s) { t->dropped++; return -1; }
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->id = id;
        s->x0_ns = t1;
        s->y0_ns = (int64_t)(t2 - t1) - (int64_t)(last_delay_ns / 2);
        t->count++;
    }
    s->last_seen_ns = now_ns;
    double x = (double)((int64_t)(t1 - s->x0_ns));
    double y = (double)((int64_t)(t2 - t1) - (int64_t)(last_delay_ns / 2) - s->y0_ns);
    if (s->samples && x <= s->last_x) return 0; /* retransmit or out-of-order */
    s->last_x = x;
    s->w = FORGET * s->w + 1.0;
    s->sxx *= FORGET;
    s->sxy *= FORGET;
    double dx = x - s->mx;
    s->mx += dx / s->w;
    double dy = y - s->my;
    s->my += dy / s->w;
    s->sxx += dx * (x - s->mx);
    s->sxy += dx * (y - s->my);
    s->samples++;
    return 0;
}

static void entry_of(const slot_t *s, drift_entry_t *e) {
    e->client_id = s->id;
    e->samples = s->samples;
    e->span_s = s->last_x / 1e9;
    double slope = s->sxx > 0 ? s->sxy / s->sxx : 0;
    e->drift_ppm = -slope * 1e6;
    e->offset_us = ((double)s->y0_ns + s->my + slope * (s->last_x - s->mx)) / 1e3;
    e->ready = s->samples >= DRIFT_MIN_SAMPLES && e->span_s >= DRIFT_MIN_SPAN_S && s->sxx > 0;
}

size_t drift_expire(drift_table_t *t, uint64_t before) {
    /* Rebuild: linear probing does not allow simple deletion. */
    slot_t *old = t->slots;
    size_t ocap = t->cap, dropped = 0;
    slot_t *ns = calloc(ocap, sizeof(slot_t));
    if (!ns) return 0;
    t->slots = ns;
    t->count = 0;
    for (size_t i = 0; i < ocap; i++) {
        if (!old[i].used) continue;
        if (old[i].last_seen_ns < before) { dropped++; continue; }
        *find(t, old[i].id, 1) = old[i];
        t->count++;
    }
    free(old);
    return dropped;
}

size_t drift_count(const drift_table_t *t) { return t->count; }
uint64_t drift_dropped(const drift_table_t *t) { return t->dropped; }

static int cmp_abs_desc(const void *a, const void *b) {
    double x = fabs(((const drift_entry_t *)a)->drift_ppm), y = fabs(((const drift_entry_t *)b)->drift_ppm);
    return x < y ? 1 : x > y ? -1 : 0;
}

size_t drift_top(const drift_table_t *t, drift_entry_t *out, size_t max) {
    size_t n = 0;
    drift_entry_t *all = malloc((t->count ? t->count : 1) * sizeof(*all));
    if (!all) return 0;
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slots[i].used) continue;
        drift_entry_t e;
        entry_of(&t->slots[i], &e);
        if (e.ready) all[n++] = e;
    }
    qsort(all, n, sizeof(*all), cmp_abs_desc);
    if (n > max) n = max;
    memcpy(out, all, n * sizeof(*all));
    free(all);
    return n;
}

void drift_summary(const drift_table_t *t, drift_summary_t *o) {
    memset(o, 0, sizeof(*o));
    double *v = malloc((t->count ? t->count : 1) * sizeof(*v));
    if (!v) return;
    size_t n = 0;
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slots[i].used) continue;
        drift_entry_t e;
        entry_of(&t->slots[i], &e);
        if (e.ready) v[n++] = fabs(e.drift_ppm);
    }
    o->ready = n;
    if (n) {
        o->abs_p50 = stats_percentile(v, n, 50);
        o->abs_p99 = stats_percentile(v, n, 99);
        o->abs_max = v[n - 1];
    }
    free(v);
}
