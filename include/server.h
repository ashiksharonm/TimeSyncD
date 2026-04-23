#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"
#include <netinet/in.h>

typedef struct {
    int mode_tcp; // 1 for tcp, 0 for udp
    int port;
    int interval_ms;
} server_config_t;

// Context for server shared memory
extern metrics_shm_t* g_metrics;

// sync_engine.c
void sync_engine_init(void);
void sync_engine_cleanup(void);
void sync_engine_update_metrics(uint64_t latency_ns);
void sync_engine_set_clients(uint32_t active_clients);
void* broadcast_thread_func(void* arg);

// network.c
int network_init(const server_config_t* config);
void network_cleanup(void);
void* accept_thread_func(void* arg);
void* worker_thread_func(void* arg);
void network_broadcast_sync(uint64_t server_ts_ns);

#endif // SERVER_H
