#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

typedef struct {
    int tcp;          /* 1 = TCP, 0 = UDP */
    int port;         /* 0 = pick an ephemeral port (see server_port) */
    int idle_sec;     /* drop clients silent for this long; <= 0 disables */
    uint32_t max_clients; /* 0 = unlimited (bounded by file descriptors) */
    int use_shm;      /* publish telemetry in POSIX shared memory */
    int backlog;
} server_config_t;

typedef struct {
    uint64_t accepted;    /* TCP connections accepted */
    uint64_t rejected;    /* refused at max_clients */
    uint64_t requests;    /* requests answered */
    uint64_t proto_errors;/* malformed frames/datagrams */
    uint64_t expired;     /* idle clients dropped */
    uint64_t accept_errors; /* e.g. EMFILE */
    uint32_t active;      /* current clients (TCP connections / UDP peers) */
} server_stats_t;

typedef struct server server_t;

server_t *server_create(const server_config_t *cfg);
int server_port(const server_t *s);
void server_run(server_t *s);   /* blocks until server_stop() */
void server_stop(server_t *s);  /* async-signal-safe, callable from any thread */
void server_get_stats(const server_t *s, server_stats_t *out);
struct metrics *server_metrics(server_t *s);
void server_destroy(server_t *s);

#endif
