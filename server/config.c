#include "../include/server.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void server_config_defaults(server_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->tcp = 1;
    c->port = 9000;
    c->idle_sec = 30;
    c->use_shm = 1;
    c->backlog = 4096;
    strcpy(c->backend, "auto");
    c->http_enabled = 1;
    c->http_port = 9100;
    c->drift_max_clients = 100000;
    c->drain_ms = 2000;
    c->drift_top = 20;
}

static int parse_int(const char *v, long lo, long hi, long *out) {
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *end || x < lo || x > hi) return -1;
    *out = x;
    return 0;
}

static int parse_bool(const char *v, int *out) {
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) { *out = 1; return 0; }
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off")) { *out = 0; return 0; }
    return -1;
}

int server_config_set(server_config_t *c, const char *k, const char *v) {
    long x;
    int b;
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "tcp")) c->tcp = 1;
        else if (!strcmp(v, "udp")) c->tcp = 0;
        else return -1;
    } else if (!strcmp(k, "port")) { if (parse_int(v, 0, 65535, &x)) return -1; c->port = (int)x; }
    else if (!strcmp(k, "idle_sec")) { if (parse_int(v, 0, 86400, &x)) return -1; c->idle_sec = (int)x; }
    else if (!strcmp(k, "max_clients")) { if (parse_int(v, 0, 10000000, &x)) return -1; c->max_clients = (uint32_t)x; }
    else if (!strcmp(k, "backlog")) { if (parse_int(v, 1, 65535, &x)) return -1; c->backlog = (int)x; }
    else if (!strcmp(k, "shm")) { if (parse_bool(v, &b)) return -1; c->use_shm = b; }
    else if (!strcmp(k, "backend")) {
        if (strcmp(v, "auto") && strcmp(v, "epoll") && strcmp(v, "poll")) return -1;
        snprintf(c->backend, sizeof c->backend, "%s", v);
    } else if (!strcmp(k, "metrics")) { if (parse_bool(v, &b)) return -1; c->http_enabled = b; }
    else if (!strcmp(k, "metrics_port")) { if (parse_int(v, 0, 65535, &x)) return -1; c->http_port = (int)x; }
    else if (!strcmp(k, "drift_max_clients")) { if (parse_int(v, 0, 10000000, &x)) return -1; c->drift_max_clients = (uint32_t)x; }
    else if (!strcmp(k, "drain_ms")) { if (parse_int(v, 0, 60000, &x)) return -1; c->drain_ms = (int)x; }
    else if (!strcmp(k, "sndbuf")) { if (parse_int(v, 0, 1 << 26, &x)) return -1; c->sndbuf = (int)x; }
    else if (!strcmp(k, "drift_top")) { if (parse_int(v, 0, 1000, &x)) return -1; c->drift_top = (int)x; }
    else return -1;
    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int server_config_load(const char *path, server_config_t *cfg, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return -1; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = trim(line);
        if (!*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) { snprintf(err, errlen, "%s:%d: expected key = value", path, n); fclose(f); return -1; }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (server_config_set(cfg, k, v) < 0) {
            snprintf(err, errlen, "%s:%d: bad key or value for '%s'", path, n, k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
