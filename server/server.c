#include "../include/metrics.h"
#include "../include/server.h"
#include "../include/utils.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static server_t *g_server;

static void on_signal(int sig) {
    (void)sig;
    if (g_server) server_stop(g_server);
}

int main(int argc, char *argv[]) {
    server_config_t cfg = {.tcp = 1, .port = 9000, .idle_sec = 30, .use_shm = 1, .backlog = 4096};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) cfg.tcp = !strcmp(argv[++i], "tcp");
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-sec") && i + 1 < argc) cfg.idle_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) cfg.max_clients = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-shm")) cfg.use_shm = 0;
        else {
            fprintf(stderr, "usage: %s [--mode tcp|udp] [--port N] [--idle-sec N] [--max-clients N] [--no-shm]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    uint64_t nofile = raise_nofile_limit();
    g_server = server_create(&cfg);
    if (!g_server) {
        LOG_ERR("failed to start server on port %d", cfg.port);
        return 1;
    }
    LOG_INFO("TimeSyncD server: %s port %d, idle timeout %ds, fd limit %llu",
             cfg.tcp ? "TCP" : "UDP", server_port(g_server), cfg.idle_sec, (unsigned long long)nofile);
    server_run(g_server);

    server_stats_t st;
    server_get_stats(g_server, &st);
    LOG_INFO("shutting down: %llu requests, %llu accepted, %llu expired, %llu protocol errors",
             (unsigned long long)st.requests, (unsigned long long)st.accepted,
             (unsigned long long)st.expired, (unsigned long long)st.proto_errors);
    server_destroy(g_server);
    return 0;
}
