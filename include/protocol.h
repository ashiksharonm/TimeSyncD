#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <pthread.h>

#define SHM_NAME "/timesyncd_metrics_v1"

typedef enum {
    MSG_REGISTER = 1,
    MSG_SYNC = 2,
    MSG_RESPONSE = 3
} msg_type_t;

// Packed structure for network transmission
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint32_t client_id;
    uint64_t server_ts_ns;
    uint64_t client_ts_ns;
} sync_message_t;

// Structure for IPC shared memory
typedef struct {
    pthread_mutex_t mutex;
    uint32_t active_clients;
    uint64_t total_latency_ns;
    uint64_t samples;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    
    // Welford's algorithm fields for jitter (variance)
    double m2;
    double mean;
} metrics_shm_t;

#endif // PROTOCOL_H
