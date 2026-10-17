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

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--config FILE] [--mode tcp|udp] [--port N] [--idle-sec N] [--max-clients N]\n"
            "          [--backlog N] [--backend auto|epoll|poll] [--metrics-port N] [--no-metrics]\n"
            "          [--drift-max-clients N] [--drain-ms N] [--no-shm]\n"
            "Command-line options override the config file (key = value lines; same names with _).\n",
            argv0);
}

int main(int argc, char *argv[]) {
    server_config_t cfg;
    server_config_defaults(&cfg);
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--config")) {
            char err[256];
            if (server_config_load(argv[i + 1], &cfg, err, sizeof err) < 0) {
                LOG_ERR("%s", err);
                return 2;
            }
        }
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config")) { i++; continue; }
        if (!strcmp(argv[i], "--no-shm")) { cfg.use_shm = 0; continue; }
        if (!strcmp(argv[i], "--no-metrics")) { cfg.http_enabled = 0; continue; }
        if (!strncmp(argv[i], "--", 2) && i + 1 < argc) {
            char key[32];
            snprintf(key, sizeof key, "%s", argv[i] + 2);
            for (char *p = key; *p; p++) if (*p == '-') *p = '_';
            if (!strcmp(key, "metrics_port")) cfg.http_enabled = 1;
            if (server_config_set(&cfg, key, argv[i + 1]) == 0) { i++; continue; }
        }
        usage(argv[0]);
        return 2;
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
    LOG_INFO("TimeSyncD server: %s port %d, %s backend, idle timeout %ds, fd limit %llu",
             cfg.tcp ? "TCP" : "UDP", server_port(g_server), server_backend(g_server), cfg.idle_sec,
             (unsigned long long)nofile);
    if (server_http_port(g_server))
        LOG_INFO("metrics on http://0.0.0.0:%d/metrics (also /metrics.json, /healthz)", server_http_port(g_server));
    server_run(g_server);

    server_stats_t st;
    server_get_stats(g_server, &st);
    LOG_INFO("shut down cleanly: %llu requests, %llu accepted, %llu expired, %llu protocol errors",
             (unsigned long long)st.requests, (unsigned long long)st.accepted,
             (unsigned long long)st.expired, (unsigned long long)st.proto_errors);
    server_destroy(g_server);
    return 0;
}
