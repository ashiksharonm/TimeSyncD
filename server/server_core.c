/*
 * Event-driven time server: a single poll() loop serves TCP or UDP clients.
 *
 *  - The client table grows dynamically; there is no FD_SETSIZE cap, so the
 *    limits are file descriptors and memory.
 *  - Every TCP client has its own frame reassembly buffer, so partial reads
 *    are completed on later wake-ups instead of dropped.
 *  - Replies that cannot be written immediately are queued per connection
 *    (POLLOUT); a client whose queue overflows is disconnected.  No lock is
 *    held while doing I/O: the only mutex is the telemetry one, taken for a
 *    few counter updates.
 *  - Idle TCP connections and silent UDP peers are expired.
 */
#include "../include/metrics.h"
#include "../include/protocol.h"
#include "../include/server.h"
#include "../include/utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
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
#define UDP_BATCH 128

typedef struct {
    int fd;
    frame_buf_t in;
    uint8_t out[FRAME_LEN * 8];
    size_t out_len;
    uint64_t last_seen;
} conn_t;

typedef struct {
    struct sockaddr_in addr;
    uint64_t last_seen;
    int used;
} peer_t;

struct server {
    server_config_t cfg;
    int sock, port;
    conn_t **conns;
    struct pollfd *pfds; /* pfds[0] = listening/UDP socket, pfds[1 + i] <-> conns[i] */
    size_t nconn, cap;
    peer_t *peers;
    size_t peer_cap, peer_count;
    uint64_t accept_resume_ns, last_sweep_ns;
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

/* ---------------- TCP connection table ---------------- */

static void conn_remove(server_t *s, size_t i) {
    conn_t *c = s->conns[i];
    close(c->fd);
    free(c);
    s->nconn--;
    s->conns[i] = s->conns[s->nconn];
    s->pfds[i + 1] = s->pfds[s->nconn + 1];
    atomic_store(&s->active, (uint32_t)s->nconn);
    metrics_set_clients(s->metrics, (uint32_t)s->nconn);
}

static int conn_add(server_t *s, int fd, uint64_t now) {
    if (s->nconn == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256;
        conn_t **nc = realloc(s->conns, ncap * sizeof(*nc));
        if (!nc) return -1;
        s->conns = nc;
        struct pollfd *np = realloc(s->pfds, (ncap + 1) * sizeof(*np));
        if (!np) return -1;
        s->pfds = np;
        s->cap = ncap;
    }
    conn_t *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->fd = fd;
    c->last_seen = now;
    s->conns[s->nconn] = c;
    s->pfds[s->nconn + 1] = (struct pollfd){.fd = fd, .events = POLLIN};
    s->nconn++;
    atomic_store(&s->active, (uint32_t)s->nconn);
    metrics_set_clients(s->metrics, (uint32_t)s->nconn);
    return 0;
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

static int make_reply(server_t *s, const msg_t *req, uint8_t out[FRAME_LEN]) {
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
    return 0;
}

static void accept_ready(server_t *s, uint64_t now) {
    for (int burst = 0; burst < 256; burst++) {
        int fd = accept(s->sock, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR) return;
            /* EMFILE/ENFILE/ENOBUFS: the pending connection stays queued, so stop polling
             * the listener briefly instead of spinning. */
            atomic_fetch_add(&s->accept_errors, 1);
            s->accept_resume_ns = now + ACCEPT_PAUSE_NS;
            return;
        }
        if (s->cfg.max_clients && s->nconn >= s->cfg.max_clients) {
            atomic_fetch_add(&s->rejected, 1);
            close(fd);
            continue;
        }
        set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (conn_add(s, fd, now) < 0) {
            close(fd);
            continue;
        }
        atomic_fetch_add(&s->accepted, 1);
    }
}

static void conn_readable(server_t *s, size_t i, uint64_t now) {
    conn_t *c = s->conns[i];
    size_t cap;
    uint8_t *dst = fb_space(&c->in, &cap);
    ssize_t n = cap ? recv(c->fd, dst, cap, 0) : -1;
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_remove(s, i);
        return;
    }
    if (n < 0) return;
    fb_commit(&c->in, (size_t)n);
    c->last_seen = now;
    msg_t m;
    int rc;
    while ((rc = fb_next(&c->in, &m)) == 1) {
        uint8_t reply[FRAME_LEN];
        if (make_reply(s, &m, reply) == 0 && conn_send(c, reply, sizeof reply) < 0) {
            conn_remove(s, i);
            return;
        }
    }
    if (rc < 0) {
        atomic_fetch_add(&s->proto_errors, 1);
        conn_remove(s, i);
        return;
    }
    s->pfds[i + 1].events = POLLIN | (c->out_len ? POLLOUT : 0);
}

static void udp_ready(server_t *s, uint64_t now) {
    for (int k = 0; k < UDP_BATCH; k++) {
        uint8_t buf[512];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(s->sock, buf, sizeof buf, 0, (struct sockaddr *)&src, &sl);
        if (n < 0) return; /* EAGAIN or error: back to poll */
        msg_t m;
        uint8_t reply[FRAME_LEN];
        if (proto_decode_datagram(buf, (size_t)n, &m) < 0) {
            atomic_fetch_add(&s->proto_errors, 1);
            continue;
        }
        if (make_reply(s, &m, reply) < 0) continue;
        peer_touch(s, &src, now);
        sendto(s->sock, reply, sizeof reply, 0, (struct sockaddr *)&src, sl);
    }
}

static void sweep(server_t *s, uint64_t now) {
    if (s->cfg.idle_sec <= 0) return;
    uint64_t idle = (uint64_t)s->cfg.idle_sec * 1000000000ULL;
    if (now < idle) return;
    uint64_t dropped = 0;
    if (s->cfg.tcp) {
        for (size_t i = s->nconn; i-- > 0;)
            if (s->conns[i]->last_seen < now - idle) { conn_remove(s, i); dropped++; }
    } else if (peers_rebuild(s, s->peer_cap, now - idle, &dropped) == 0 && dropped) {
        atomic_store(&s->active, (uint32_t)s->peer_count);
        metrics_set_clients(s->metrics, (uint32_t)s->peer_count);
    }
    if (dropped) {
        atomic_fetch_add(&s->expired, dropped);
        metrics_add_expired(s->metrics, dropped);
    }
}

/* ---------------- lifecycle ---------------- */

server_t *server_create(const server_config_t *cfg) {
    server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    s->sock = -1;
    s->metrics = metrics_open(cfg->use_shm);
    if (!s->metrics) goto fail;
    s->sock = socket(AF_INET, cfg->tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s->sock < 0) goto fail;
    int one = 1;
    setsockopt(s->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)cfg->port);
    if (bind(s->sock, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;
    if (cfg->tcp && listen(s->sock, cfg->backlog > 0 ? cfg->backlog : 1024) < 0) goto fail;
    socklen_t al = sizeof(a);
    getsockname(s->sock, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    set_nonblocking(s->sock);
    s->pfds = malloc(sizeof(struct pollfd));
    if (!s->pfds) goto fail;
    s->pfds[0] = (struct pollfd){.fd = s->sock, .events = POLLIN};
    if (!cfg->tcp) {
        s->peer_cap = 1024;
        s->peers = calloc(s->peer_cap, sizeof(peer_t));
        if (!s->peers) goto fail;
    }
    return s;
fail:
    server_destroy(s);
    return NULL;
}

int server_port(const server_t *s) { return s->port; }
void server_stop(server_t *s) { atomic_store(&s->stop, 1); }
struct metrics *server_metrics(server_t *s) { return s->metrics; }

void server_get_stats(const server_t *s, server_stats_t *o) {
    o->accepted = atomic_load(&s->accepted);
    o->rejected = atomic_load(&s->rejected);
    o->requests = atomic_load(&s->requests);
    o->proto_errors = atomic_load(&s->proto_errors);
    o->expired = atomic_load(&s->expired);
    o->accept_errors = atomic_load(&s->accept_errors);
    o->active = atomic_load(&s->active);
}

void server_run(server_t *s) {
    while (!atomic_load(&s->stop)) {
        uint64_t now = get_time_ns();
        s->pfds[0].events = now < s->accept_resume_ns ? 0 : POLLIN;
        int r = poll(s->pfds, (nfds_t)(s->nconn + 1), POLL_TIMEOUT_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        now = get_time_ns();
        if (r > 0) {
            if (s->pfds[0].revents & POLLIN) {
                if (s->cfg.tcp) accept_ready(s, now);
                else udp_ready(s, now);
            }
            for (size_t i = s->nconn; i-- > 0;) {
                short ev = s->pfds[i + 1].revents;
                if (!ev) continue;
                if (ev & (POLLERR | POLLNVAL)) { conn_remove(s, i); continue; }
                if (ev & POLLOUT) {
                    if (conn_flush(s->conns[i]) < 0) { conn_remove(s, i); continue; }
                    s->pfds[i + 1].events = POLLIN | (s->conns[i]->out_len ? POLLOUT : 0);
                }
                if (ev & (POLLIN | POLLHUP)) conn_readable(s, i, now);
            }
        }
        if (now - s->last_sweep_ns >= SWEEP_NS) {
            s->last_sweep_ns = now;
            sweep(s, now);
        }
    }
}

void server_destroy(server_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->nconn; i++) {
        close(s->conns[i]->fd);
        free(s->conns[i]);
    }
    free(s->conns);
    free(s->pfds);
    free(s->peers);
    if (s->sock >= 0) close(s->sock);
    metrics_close(s->metrics);
    free(s);
}
