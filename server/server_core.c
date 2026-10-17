/*
 * Event-driven time server: one thread, one readiness loop (epoll on Linux,
 * poll() as the fallback or by choice), non-blocking sockets throughout.
 *
 *  - The client table grows dynamically; there is no FD_SETSIZE cap, so the
 *    limits are file descriptors and memory.
 *  - Every TCP client has its own frame reassembly buffer, so partial reads
 *    are completed on later wake-ups instead of dropped.
 *  - Replies that cannot be written immediately are queued per connection
 *    (write interest); a client whose queue overflows is disconnected.  No
 *    lock is held while doing I/O: the only mutex is the telemetry one, taken
 *    for a few counter updates.
 *  - Idle TCP connections and silent UDP peers are expired.
 *  - Optional HTTP listener serving /metrics (Prometheus), /metrics.json and
 *    /healthz through the same loop.
 *  - On stop the listeners close and queued replies are flushed for up to
 *    drain_ms before the server returns.
 */
#include "../include/drift.h"
#include "../include/exposition.h"
#include "../include/metrics.h"
#include "../include/poller.h"
#include "../include/protocol.h"
#include "../include/server.h"
#include "../include/utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define POLL_TIMEOUT_MS 100
#define SWEEP_NS 1000000000ULL
#define ACCEPT_PAUSE_NS 100000000ULL
#define HTTP_IDLE_NS 5000000000ULL
#define UDP_BATCH 128
#define MAX_EVENTS 256
#define HTTP_REQ_MAX 1024

typedef enum { CK_TIME, CK_HTTP } conn_kind_t;

typedef struct {
    char req[HTTP_REQ_MAX];
    size_t req_len;
    char *resp;
    size_t resp_len, resp_off;
} http_t;

typedef struct {
    conn_kind_t kind;
    int fd;
    size_t idx;      /* position in server.conns */
    int want_write;  /* current poller interest */
    frame_buf_t in;
    uint8_t out[FRAME_LEN * 8];
    size_t out_len;
    uint64_t last_seen;
    http_t *http;
} conn_t;

typedef struct {
    struct sockaddr_in addr;
    uint64_t last_seen;
    int used;
} peer_t;

struct server {
    server_config_t cfg;
    int sock, port;
    int http_sock, http_port;
    poller_t *poller;
    char sock_tag, http_tag; /* addresses identify the listeners in poller events */
    int accept_paused;
    conn_t **conns;
    size_t nconn, ntime, cap;
    peer_t *peers;
    size_t peer_cap, peer_count;
    uint64_t accept_resume_ns, last_sweep_ns, start_ns;
    drift_table_t *drift;
    atomic_int stop;
    _Atomic uint64_t accepted, rejected, requests, proto_errors, expired, accept_errors;
    _Atomic uint32_t active;
    metrics_t *metrics;
};

/* ---------------- UDP peer table (open addressing) ---------------- */

static size_t peer_hash(const struct sockaddr_in *a, size_t mask) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)&a->sin_addr;
    for (int i = 0; i < 4; i++) h = (h ^ p[i]) * 1099511628211ULL;
    h = (h ^ (a->sin_port & 0xFF)) * 1099511628211ULL;
    h = (h ^ (a->sin_port >> 8)) * 1099511628211ULL;
    return (size_t)(h ^ (h >> 29)) & mask;
}

static peer_t *peer_find(peer_t *tab, size_t cap, const struct sockaddr_in *a, int *found) {
    size_t i = peer_hash(a, cap - 1);
    for (;;) {
        peer_t *p = &tab[i];
        if (!p->used) { *found = 0; return p; }
        if (p->addr.sin_addr.s_addr == a->sin_addr.s_addr && p->addr.sin_port == a->sin_port) {
            *found = 1;
            return p;
        }
        i = (i + 1) & (cap - 1);
    }
}

static int peers_rebuild(server_t *s, size_t new_cap, uint64_t drop_before, uint64_t *dropped) {
    peer_t *tab = calloc(new_cap, sizeof(peer_t));
    if (!tab) return -1;
    size_t count = 0;
    for (size_t i = 0; i < s->peer_cap; i++) {
        peer_t *p = &s->peers[i];
        if (!p->used) continue;
        if (p->last_seen < drop_before) { if (dropped) (*dropped)++; continue; }
        int found;
        *peer_find(tab, new_cap, &p->addr, &found) = *p;
        count++;
    }
    free(s->peers);
    s->peers = tab;
    s->peer_cap = new_cap;
    s->peer_count = count;
    return 0;
}

static void peer_touch(server_t *s, const struct sockaddr_in *a, uint64_t now) {
    if ((s->peer_count + 1) * 2 > s->peer_cap && peers_rebuild(s, s->peer_cap * 2, 0, NULL) < 0)
        return;
    int found;
    peer_t *p = peer_find(s->peers, s->peer_cap, a, &found);
    p->last_seen = now;
    if (!found) {
        p->addr = *a;
        p->used = 1;
        s->peer_count++;
        atomic_store(&s->active, (uint32_t)s->peer_count);
        metrics_set_clients(s->metrics, (uint32_t)s->peer_count);
    }
}

/* ---------------- connection table ---------------- */

static void set_active(server_t *s) {
    atomic_store(&s->active, (uint32_t)s->ntime);
    metrics_set_clients(s->metrics, (uint32_t)s->ntime);
}

static void conn_remove(server_t *s, conn_t *c) {
    poller_del(s->poller, c->fd);
    close(c->fd);
    if (c->kind == CK_TIME) s->ntime--;
    s->nconn--;
    s->conns[c->idx] = s->conns[s->nconn];
    s->conns[c->idx]->idx = c->idx;
    if (c->http) { free(c->http->resp); free(c->http); }
    free(c);
    set_active(s);
}

static conn_t *conn_add(server_t *s, int fd, conn_kind_t kind, uint64_t now) {
    if (s->nconn == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256;
        conn_t **nc = realloc(s->conns, ncap * sizeof(*nc));
        if (!nc) return NULL;
        s->conns = nc;
        s->cap = ncap;
    }
    conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (kind == CK_HTTP && !(c->http = calloc(1, sizeof(http_t)))) { free(c); return NULL; }
    c->kind = kind;
    c->fd = fd;
    c->last_seen = now;
    if (poller_add(s->poller, fd, c, PE_READ) < 0) { free(c->http); free(c); return NULL; }
    c->idx = s->nconn;
    s->conns[s->nconn++] = c;
    if (kind == CK_TIME) { s->ntime++; set_active(s); }
    return c;
}

static void conn_update_interest(server_t *s, conn_t *c, int want_write, int want_read) {
    if (c->want_write == want_write && want_read) return;
    c->want_write = want_write;
    poller_set(s->poller, c->fd, c, (want_read ? PE_READ : 0) | (want_write ? PE_WRITE : 0));
}

/* Returns -1 if the connection must be closed. */
static int conn_send(conn_t *c, const uint8_t *data, size_t len) {
    if (c->out_len == 0) {
        ssize_t n = send(c->fd, data, len, SEND_FLAGS);
        if (n == (ssize_t)len) return 0;
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
            n = 0;
        }
        data += n;
        len -= (size_t)n;
    }
    if (c->out_len + len > sizeof(c->out)) return -1; /* client is not reading */
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return 0;
}

static int conn_flush(conn_t *c) {
    if (c->out_len == 0) return 0;
    ssize_t n = send(c->fd, c->out, c->out_len, SEND_FLAGS);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    memmove(c->out, c->out + n, c->out_len - (size_t)n);
    c->out_len -= (size_t)n;
    return 0;
}

/* ---------------- request handling ---------------- */

static int make_reply(server_t *s, const msg_t *req, uint8_t out[FRAME_LEN], uint64_t now) {
    if (req->type != MSG_REQUEST) {
        atomic_fetch_add(&s->proto_errors, 1);
        return -1;
    }
    msg_t rep = *req;
    rep.type = MSG_REPLY;
    rep.t2 = get_time_ns();
    rep.t3 = get_time_ns();
    proto_encode_frame(&rep, out);
    atomic_fetch_add(&s->requests, 1);
    metrics_record_request(s->metrics, req->last_delay_ns);
    if (s->drift) drift_observe(s->drift, req->client_id, req->t1, rep.t2, req->last_delay_ns, now);
    return 0;
}

static void set_sock_opts(int fd) {
    set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

static void accept_ready(server_t *s, int lfd, conn_kind_t kind, uint64_t now) {
    for (int burst = 0; burst < 256; burst++) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR) return;
            /* EMFILE/ENFILE/ENOBUFS: the pending connection stays queued, so stop watching
             * the listeners briefly instead of spinning. */
            atomic_fetch_add(&s->accept_errors, 1);
            s->accept_resume_ns = now + ACCEPT_PAUSE_NS;
            return;
        }
        if (kind == CK_TIME && s->cfg.max_clients && s->ntime >= s->cfg.max_clients) {
            atomic_fetch_add(&s->rejected, 1);
            close(fd);
            continue;
        }
        set_sock_opts(fd);
        if (!conn_add(s, fd, kind, now)) {
            close(fd);
            continue;
        }
        if (kind == CK_TIME) atomic_fetch_add(&s->accepted, 1);
    }
}

static void conn_readable(server_t *s, conn_t *c, uint64_t now) {
    size_t cap;
    uint8_t *dst = fb_space(&c->in, &cap);
    ssize_t n = cap ? recv(c->fd, dst, cap, 0) : -1;
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_remove(s, c);
        return;
    }
    if (n < 0) return;
    fb_commit(&c->in, (size_t)n);
    c->last_seen = now;
    msg_t m;
    int rc;
    while ((rc = fb_next(&c->in, &m)) == 1) {
        uint8_t reply[FRAME_LEN];
        if (make_reply(s, &m, reply, now) == 0 && conn_send(c, reply, sizeof reply) < 0) {
            conn_remove(s, c);
            return;
        }
    }
    if (rc < 0) {
        atomic_fetch_add(&s->proto_errors, 1);
        conn_remove(s, c);
        return;
    }
    conn_update_interest(s, c, c->out_len > 0, 1);
}

static void udp_ready(server_t *s, uint64_t now) {
    for (int k = 0; k < UDP_BATCH; k++) {
        uint8_t buf[512];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(s->sock, buf, sizeof buf, 0, (struct sockaddr *)&src, &sl);
        if (n < 0) return; /* EAGAIN or error: back to the poller */
        msg_t m;
        uint8_t reply[FRAME_LEN];
        if (proto_decode_datagram(buf, (size_t)n, &m) < 0) {
            atomic_fetch_add(&s->proto_errors, 1);
            continue;
        }
        if (make_reply(s, &m, reply, now) < 0) continue;
        peer_touch(s, &src, now);
        sendto(s->sock, reply, sizeof reply, 0, (struct sockaddr *)&src, sl);
    }
}

/* ---------------- HTTP metrics endpoint ---------------- */

void server_get_stats(const server_t *s, server_stats_t *o) {
    o->accepted = atomic_load(&s->accepted);
    o->rejected = atomic_load(&s->rejected);
    o->requests = atomic_load(&s->requests);
    o->proto_errors = atomic_load(&s->proto_errors);
    o->expired = atomic_load(&s->expired);
    o->accept_errors = atomic_load(&s->accept_errors);
    o->active = atomic_load(&s->active);
}

#define RESP_MAX (96 * 1024)

static char *render(server_t *s, const char *path, size_t *len, const char **ctype, int *status) {
    *status = 200;
    char *body = malloc(RESP_MAX);
    char *resp = malloc(RESP_MAX + 256);
    if (!body || !resp) { free(body); free(resp); return NULL; }
    size_t blen = 0;
    if (!strcmp(path, "/healthz")) {
        blen = (size_t)snprintf(body, RESP_MAX, "ok\n");
        *ctype = "text/plain";
    } else if (!strcmp(path, "/metrics") || !strcmp(path, "/metrics.json")) {
        expo_t e;
        memset(&e, 0, sizeof e);
        server_get_stats(s, &e.srv);
        metrics_snapshot(s->metrics, &e.m);
        e.backend = poller_name(s->poller);
        e.tcp = s->cfg.tcp;
        e.uptime_s = (double)(get_time_ns() - s->start_ns) / 1e9;
        drift_entry_t *top = NULL;
        if (s->drift) {
            e.drift_tracked = drift_count(s->drift);
            e.drift_dropped = drift_dropped(s->drift);
            drift_summary(s->drift, &e.drift);
        }
        int json = !strcmp(path, "/metrics.json");
        if (json && s->drift && s->cfg.drift_top > 0 && (top = malloc((size_t)s->cfg.drift_top * sizeof *top))) {
            e.top = top;
            e.ntop = drift_top(s->drift, top, (size_t)s->cfg.drift_top);
        }
        blen = json ? expo_json(body, RESP_MAX, &e) : expo_prometheus(body, RESP_MAX, &e);
        *ctype = json ? "application/json" : "text/plain; version=0.0.4";
        free(top);
    } else {
        *status = 404;
        blen = (size_t)snprintf(body, RESP_MAX, "not found\n");
        *ctype = "text/plain";
    }
    int hl = snprintf(resp, 256, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      *status, *status == 200 ? "OK" : "Not Found", *ctype, blen);
    memcpy(resp + hl, body, blen);
    free(body);
    *len = (size_t)hl + blen;
    return resp;
}

static void http_send_more(server_t *s, conn_t *c) {
    http_t *h = c->http;
    while (h->resp_off < h->resp_len) {
        ssize_t n = send(c->fd, h->resp + h->resp_off, h->resp_len - h->resp_off, SEND_FLAGS);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                conn_update_interest(s, c, 1, 0);
                return;
            }
            break;
        }
        h->resp_off += (size_t)n;
    }
    conn_remove(s, c); /* done, or failed: Connection: close */
}

static void http_readable(server_t *s, conn_t *c, uint64_t now) {
    http_t *h = c->http;
    ssize_t n = recv(c->fd, h->req + h->req_len, HTTP_REQ_MAX - 1 - h->req_len, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_remove(s, c);
        return;
    }
    if (n < 0) return;
    h->req_len += (size_t)n;
    h->req[h->req_len] = 0;
    c->last_seen = now;
    if (!strstr(h->req, "\r\n\r\n") && !strstr(h->req, "\n\n") && h->req_len < HTTP_REQ_MAX - 1) return;
    char method[8] = "", path[128] = "";
    if (sscanf(h->req, "%7s %127s", method, path) != 2 || strcmp(method, "GET")) {
        conn_remove(s, c);
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    const char *ct;
    int st;
    h->resp = render(s, path, &h->resp_len, &ct, &st);
    if (!h->resp) { conn_remove(s, c); return; }
    http_send_more(s, c);
}

static void sweep(server_t *s, uint64_t now) {
    uint64_t idle = s->cfg.idle_sec > 0 ? (uint64_t)s->cfg.idle_sec * 1000000000ULL : 0;
    uint64_t dropped = 0;
    for (size_t i = s->nconn; i-- > 0;) {
        conn_t *c = s->conns[i];
        if (c->kind == CK_HTTP) {
            if (now > HTTP_IDLE_NS && c->last_seen < now - HTTP_IDLE_NS) conn_remove(s, c);
        } else if (idle && now > idle && s->cfg.tcp && c->last_seen < now - idle) {
            conn_remove(s, c);
            dropped++;
        }
    }
    if (idle && now > idle) {
        if (!s->cfg.tcp && peers_rebuild(s, s->peer_cap, now - idle, &dropped) == 0 && dropped) {
            atomic_store(&s->active, (uint32_t)s->peer_count);
            metrics_set_clients(s->metrics, (uint32_t)s->peer_count);
        }
        if (s->drift) drift_expire(s->drift, now - idle);
    }
    if (dropped) {
        atomic_fetch_add(&s->expired, dropped);
        metrics_add_expired(s->metrics, dropped);
    }
}

/* ---------------- lifecycle ---------------- */

static int open_listener(int type, int port, int backlog, int *bound) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || (type == SOCK_STREAM && listen(fd, backlog) < 0)) {
        close(fd);
        return -1;
    }
    socklen_t al = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    *bound = ntohs(a.sin_port);
    set_nonblocking(fd);
    return fd;
}

server_t *server_create(const server_config_t *cfg) {
    server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    s->sock = s->http_sock = -1;
    s->start_ns = get_time_ns();
    s->metrics = metrics_open(cfg->use_shm);
    if (!s->metrics) goto fail;
    s->poller = poller_create(cfg->backend[0] ? cfg->backend : "auto");
    if (!s->poller) { LOG_ERR("poller backend '%s' is not available", cfg->backend); goto fail; }
    s->sock = open_listener(cfg->tcp ? SOCK_STREAM : SOCK_DGRAM, cfg->port, cfg->backlog > 0 ? cfg->backlog : 1024, &s->port);
    if (s->sock < 0 || poller_add(s->poller, s->sock, &s->sock_tag, PE_READ) < 0) goto fail;
    if (cfg->http_enabled) {
        s->http_sock = open_listener(SOCK_STREAM, cfg->http_port, 128, &s->http_port);
        if (s->http_sock < 0 || poller_add(s->poller, s->http_sock, &s->http_tag, PE_READ) < 0) goto fail;
    }
    if (!cfg->tcp) {
        s->peer_cap = 1024;
        s->peers = calloc(s->peer_cap, sizeof(peer_t));
        if (!s->peers) goto fail;
    }
    if (cfg->drift_max_clients && !(s->drift = drift_create(cfg->drift_max_clients))) goto fail;
    return s;
fail:
    server_destroy(s);
    return NULL;
}

int server_port(const server_t *s) { return s->port; }
int server_http_port(const server_t *s) { return s->http_sock >= 0 ? s->http_port : 0; }
const char *server_backend(const server_t *s) { return poller_name(s->poller); }
void server_stop(server_t *s) { atomic_store(&s->stop, 1); }
struct metrics *server_metrics(server_t *s) { return s->metrics; }

static void dispatch(server_t *s, const pevent_t *ev, uint64_t now) {
    if (ev->ud == &s->sock_tag) {
        if (s->cfg.tcp) accept_ready(s, s->sock, CK_TIME, now);
        else udp_ready(s, now);
        return;
    }
    if (ev->ud == &s->http_tag) {
        accept_ready(s, s->http_sock, CK_HTTP, now);
        return;
    }
    conn_t *c = ev->ud;
    if (ev->error) { conn_remove(s, c); return; }
    if (ev->writable) {
        if (c->kind == CK_HTTP) { http_send_more(s, c); return; }
        if (conn_flush(c) < 0) { conn_remove(s, c); return; }
        conn_update_interest(s, c, c->out_len > 0, 1);
    }
    if (ev->readable) {
        if (c->kind == CK_HTTP) http_readable(s, c, now);
        else conn_readable(s, c, now);
    }
}

/* Stop accepting and give queued replies a bounded time to leave. */
static void drain(server_t *s) {
    if (s->sock >= 0) { poller_del(s->poller, s->sock); close(s->sock); s->sock = -1; }
    if (s->http_sock >= 0) { poller_del(s->poller, s->http_sock); close(s->http_sock); s->http_sock = -1; }
    uint64_t deadline = get_time_ns() + (uint64_t)s->cfg.drain_ms * 1000000ULL;
    size_t pending = 0;
    for (size_t i = 0; i < s->nconn; i++) {
        conn_t *c = s->conns[i];
        int need = c->out_len > 0 || (c->http && c->http->resp);
        pending += need;
        c->want_write = need;
        poller_set(s->poller, c->fd, c, need ? PE_WRITE : 0);
    }
    pevent_t ev[MAX_EVENTS];
    while (pending && get_time_ns() < deadline) {
        int r = poller_wait(s->poller, ev, MAX_EVENTS, 20);
        for (int i = 0; i < r; i++) {
            conn_t *c = ev[i].ud;
            if (ev[i].error) { conn_remove(s, c); continue; }
            if (!ev[i].writable) continue;
            if (c->kind == CK_HTTP) { http_send_more(s, c); continue; }
            if (conn_flush(c) < 0 || c->out_len == 0) {
                if (c->out_len == 0) poller_set(s->poller, c->fd, c, 0);
                else conn_remove(s, c);
            }
        }
        pending = 0;
        for (size_t i = 0; i < s->nconn; i++)
            pending += s->conns[i]->out_len > 0 || (s->conns[i]->http && s->conns[i]->http->resp);
    }
}

void server_run(server_t *s) {
    pevent_t ev[MAX_EVENTS];
    while (!atomic_load(&s->stop)) {
        uint64_t now = get_time_ns();
        int paused = now < s->accept_resume_ns;
        if (paused != s->accept_paused) {
            s->accept_paused = paused;
            int want = paused ? 0 : PE_READ;
            poller_set(s->poller, s->sock, &s->sock_tag, want);
            if (s->http_sock >= 0) poller_set(s->poller, s->http_sock, &s->http_tag, want);
        }
        int r = poller_wait(s->poller, ev, MAX_EVENTS, POLL_TIMEOUT_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("%s wait failed with %zu connections: %s", poller_name(s->poller), s->nconn, strerror(errno));
            break;
        }
        now = get_time_ns();
        for (int i = 0; i < r; i++) dispatch(s, &ev[i], now);
        if (now - s->last_sweep_ns >= SWEEP_NS) {
            s->last_sweep_ns = now;
            sweep(s, now);
        }
    }
    drain(s);
}

void server_destroy(server_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->nconn; i++) {
        close(s->conns[i]->fd);
        if (s->conns[i]->http) { free(s->conns[i]->http->resp); free(s->conns[i]->http); }
        free(s->conns[i]);
    }
    free(s->conns);
    free(s->peers);
    drift_destroy(s->drift);
    if (s->sock >= 0) close(s->sock);
    if (s->http_sock >= 0) close(s->http_sock);
    poller_destroy(s->poller);
    metrics_close(s->metrics);
    free(s);
}
