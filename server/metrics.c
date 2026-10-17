#include "../include/metrics.h"
#include "../include/utils.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

const uint32_t rtt_bucket_us[RTT_BUCKETS - 1] = {50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 100000};

struct metrics {
    metrics_shm_t *shm;
    int fd;
    int is_shm;
};

metrics_t *metrics_open(int use_shm) {
    metrics_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->fd = -1;
    if (use_shm) {
        shm_unlink(SHM_NAME);
        m->fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
        if (m->fd < 0 || ftruncate(m->fd, sizeof(metrics_shm_t)) != 0) {
            LOG_ERR("shared memory create failed");
            goto fail;
        }
        void *p = mmap(NULL, sizeof(metrics_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
        if (p == MAP_FAILED)
            goto fail;
        m->shm = p;
        m->is_shm = 1;
    } else {
        m->shm = calloc(1, sizeof(metrics_shm_t));
        if (!m->shm)
            goto fail;
    }
    memset(m->shm, 0, sizeof(*m->shm));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&m->shm->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    m->shm->min_latency_ns = UINT64_MAX;
    return m;
fail:
    if (m->fd >= 0)
        close(m->fd);
    free(m);
    return NULL;
}

void metrics_close(metrics_t *m) {
    if (!m)
        return;
    pthread_mutex_destroy(&m->shm->mutex);
    if (m->is_shm) {
        munmap(m->shm, sizeof(metrics_shm_t));
        close(m->fd);
        shm_unlink(SHM_NAME);
    } else {
        free(m->shm);
    }
    free(m);
}

void metrics_record_request(metrics_t *m, uint64_t rtt_ns) {
    metrics_shm_t *s = m->shm;
    pthread_mutex_lock(&s->mutex);
    s->requests++;
    if (rtt_ns > 0) {
        uint64_t lat = rtt_ns / 2;
        if (lat < s->min_latency_ns) s->min_latency_ns = lat;
        if (lat > s->max_latency_ns) s->max_latency_ns = lat;
        welford_add(&s->latency, (double)lat);
        int b = 0;
        while (b < RTT_BUCKETS - 1 && rtt_ns / 1000 > rtt_bucket_us[b]) b++;
        s->rtt_hist[b]++;
        s->rtt_sum_ns += rtt_ns;
        s->rtt_count++;
    }
    pthread_mutex_unlock(&s->mutex);
}

void metrics_set_clients(metrics_t *m, uint32_t active) {
    pthread_mutex_lock(&m->shm->mutex);
    m->shm->active_clients = active;
    pthread_mutex_unlock(&m->shm->mutex);
}

void metrics_add_expired(metrics_t *m, uint64_t n) {
    pthread_mutex_lock(&m->shm->mutex);
    m->shm->expired += n;
    pthread_mutex_unlock(&m->shm->mutex);
}

void metrics_snapshot(metrics_t *m, metrics_shm_t *out) {
    pthread_mutex_lock(&m->shm->mutex);
    *out = *m->shm;
    pthread_mutex_unlock(&m->shm->mutex);
}
