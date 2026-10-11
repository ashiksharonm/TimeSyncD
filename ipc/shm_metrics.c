/* Reads the server's shared-memory telemetry block and prints it every 2 s. */
#include "../include/metrics.h"
#include "../include/utils.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd < 0) {
        LOG_ERR("cannot open %s. Is the server running?", SHM_NAME);
        return 1;
    }
    metrics_shm_t *shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        LOG_ERR("cannot map shared memory");
        return 1;
    }
    LOG_INFO("monitoring %s", SHM_NAME);
    for (;;) {
        sleep(2);
        pthread_mutex_lock(&shm->mutex);
        metrics_shm_t s = *shm;
        pthread_mutex_unlock(&shm->mutex);
        if (s.latency.n) {
            printf("Clients: %u | Requests: %llu | Expired: %llu | Latency avg %.1fus min %.1fus max %.1fus | Jitter %.1fus\n",
                   s.active_clients, (unsigned long long)s.requests, (unsigned long long)s.expired,
                   s.latency.mean / 1e3, (double)s.min_latency_ns / 1e3, (double)s.max_latency_ns / 1e3,
                   welford_stddev(&s.latency) / 1e3);
        } else {
            printf("Clients: %u | Requests: %llu | waiting for latency samples...\n", s.active_clients,
                   (unsigned long long)s.requests);
        }
        fflush(stdout);
    }
}
