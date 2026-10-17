#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int tcp;          /* 1 = TCP, 0 = UDP */
    int port;         /* 0 = pick an ephemeral port (see server_port) */
    int idle_sec;     /* drop clients silent for this long; <= 0 disables */
    uint32_t max_clients; /* 0 = unlimited (bounded by file descriptors) */
    int use_shm;      /* publish telemetry in POSIX shared memory */
    int backlog;
    char backend[16]; /* "auto" (default when empty), "epoll" or "poll" */
    int http_enabled; /* serve /metrics, /metrics.json and /healthz */
    int http_port;    /* 0 = ephemeral (see server_http_port) */
    uint32_t drift_max_clients; /* clients tracked for drift; 0 disables tracking */
    int drain_ms;     /* on stop: keep flushing queued replies for up to this long */
    int drift_top;    /* clients listed in /metrics.json (largest |drift| first) */
} server_config_t;

/* Fills cfg with the daemon defaults. */
void server_config_defaults(server_config_t *cfg);
/* Reads "key = value" lines ('#' comments).  Returns 0, or -1 with a message in err. */
int server_config_load(const char *path, server_config_t *cfg, char *err, size_t errlen);
/* Applies one key/value pair (the same keys as the file). Returns 0 or -1 (unknown key / bad value). */
int server_config_set(server_config_t *cfg, const char *key, const char *value);

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
int server_http_port(const server_t *s); /* 0 if the endpoint is disabled */
const char *server_backend(const server_t *s);
void server_run(server_t *s);   /* blocks until server_stop() */
void server_stop(server_t *s);  /* async-signal-safe, callable from any thread */
void server_get_stats(const server_t *s, server_stats_t *out);
struct metrics *server_metrics(server_t *s);
void server_destroy(server_t *s);

#endif
