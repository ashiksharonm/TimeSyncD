#include "../include/server.h"
#include "../include/utils.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

metrics_shm_t* g_metrics = NULL;
int g_shm_fd = -1;
extern int g_server_running;
extern server_config_t g_config;

void sync_engine_init(void) {
    shm_unlink(SHM_NAME); // Remove old one if exists
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0) {
        LOG_ERR("Failed to create shared memory!");
        exit(1);
    }
    ftruncate(g_shm_fd, sizeof(metrics_shm_t));
    g_metrics = mmap(NULL, sizeof(metrics_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_metrics == MAP_FAILED) {
        LOG_ERR("Failed to map shared memory!");
        exit(1);
    }
    
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g_metrics->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    g_metrics->active_clients = 0;
    g_metrics->total_latency_ns = 0;
    g_metrics->samples = 0;
    g_metrics->min_latency_ns = UINT64_MAX;
    g_metrics->max_latency_ns = 0;
    g_metrics->mean = 0.0;
    g_metrics->m2 = 0.0;
}

void sync_engine_cleanup(void) {
    if (g_metrics && g_metrics != MAP_FAILED) {
        pthread_mutex_destroy(&g_metrics->mutex);
        munmap(g_metrics, sizeof(metrics_shm_t));
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
    }
    shm_unlink(SHM_NAME);
}

void sync_engine_update_metrics(uint64_t latency_ns) {
    if (!g_metrics) return;
    
    pthread_mutex_lock(&g_metrics->mutex);
    g_metrics->total_latency_ns += latency_ns;
    g_metrics->samples++;
    if (latency_ns < g_metrics->min_latency_ns) g_metrics->min_latency_ns = latency_ns;
    if (latency_ns > g_metrics->max_latency_ns) g_metrics->max_latency_ns = latency_ns;
    
    // Welford's online algorithm for computing variance
    double delta = (double)latency_ns - g_metrics->mean;
    g_metrics->mean += delta / g_metrics->samples;
    double delta2 = (double)latency_ns - g_metrics->mean;
    g_metrics->m2 += delta * delta2;
    
    pthread_mutex_unlock(&g_metrics->mutex);
}

void sync_engine_set_clients(uint32_t active_clients) {
    if (!g_metrics) return;
    pthread_mutex_lock(&g_metrics->mutex);
    g_metrics->active_clients = active_clients;
    pthread_mutex_unlock(&g_metrics->mutex);
}

void* broadcast_thread_func(void* arg) {
    (void)arg;
    LOG_INFO("Broadcast thread started, interval: %d ms", g_config.interval_ms);
    uint32_t sleep_us = g_config.interval_ms * 1000;
    
    while (g_server_running) {
        usleep(sleep_us);
        uint64_t now = get_time_ns();
        network_broadcast_sync(now);
    }
    return NULL;
}
