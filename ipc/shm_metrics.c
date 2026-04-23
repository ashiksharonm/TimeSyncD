#include "../include/protocol.h"
#include "../include/utils.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <math.h>

int main(void) {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        LOG_ERR("Failed to open shared memory %s. Is the server running?", SHM_NAME);
        return 1;
    }
    
    metrics_shm_t *shm = mmap(NULL, sizeof(metrics_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        LOG_ERR("Failed to map shared memory");
        return 1;
    }

    LOG_INFO("Starting metrics reader. Monitoring %s...\n", SHM_NAME);

    while (1) {
        sleep(2);
        
        pthread_mutex_lock(&shm->mutex);
        uint32_t clients = shm->active_clients;
        uint64_t total = shm->total_latency_ns;
        uint64_t samples = shm->samples;
        uint64_t min_l = shm->min_latency_ns;
        uint64_t max_l = shm->max_latency_ns;
        double m2 = shm->m2;
        pthread_mutex_unlock(&shm->mutex);
        
        double variance = (samples > 1) ? m2 / (samples - 1) : 0.0;
        double jitter = sqrt(variance);
        
        if (samples > 0) {
            uint64_t avg = total / samples;
            printf("Clients: %u | Avg Latency: %lluus | Min: %lluus | Max: %lluus | Jitter: %.2fus\n",
                clients, (unsigned long long)avg / 1000, 
                (unsigned long long)min_l / 1000, 
                (unsigned long long)max_l / 1000, 
                jitter / 1000.0);
        } else {
            printf("Clients: %u | Waiting for samples...\n", clients);
        }
    }
    
    return 0;
}
