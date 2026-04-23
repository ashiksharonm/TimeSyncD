#include "../include/server.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

int g_server_running = 1;
server_config_t g_config;

void handle_sigint(int sig) {
    (void)sig;
    LOG_INFO("SIGINT received, shutting down gracefully...");
    g_server_running = 0;
}

int main(int argc, char* argv[]) {
    // defaults
    g_config.mode_tcp = 1;
    g_config.port = 9000;
    g_config.interval_ms = 100; // default 100ms to avoid overwhelming logs if not specified
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_config.mode_tcp = (strcmp(argv[++i], "tcp") == 0);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            g_config.interval_ms = atoi(argv[++i]);
        }
    }
    
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE on disconnected sockets
    
    LOG_INFO("Starting TimeSyncD Server...");
    LOG_INFO("Mode: %s", g_config.mode_tcp ? "TCP" : "UDP");
    LOG_INFO("Port: %d", g_config.port);
    LOG_INFO("Interval: %d ms", g_config.interval_ms);
    
    sync_engine_init();
    if (network_init(&g_config) < 0) {
        LOG_ERR("Network initialization failed.");
        sync_engine_cleanup();
        return 1;
    }
    
    pthread_t accept_thread, worker_thread, broadcast_thread;
    
    if (g_config.mode_tcp) {
        pthread_create(&accept_thread, NULL, accept_thread_func, NULL);
    }
    pthread_create(&worker_thread, NULL, worker_thread_func, NULL);
    pthread_create(&broadcast_thread, NULL, broadcast_thread_func, NULL);
    
    if (g_config.mode_tcp) {
        pthread_join(accept_thread, NULL);
    }
    pthread_join(worker_thread, NULL);
    pthread_join(broadcast_thread, NULL);
    
    network_cleanup();
    sync_engine_cleanup();
    
    LOG_INFO("Server shut down successfully.");
    return 0;
}
