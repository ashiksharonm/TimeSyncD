/*
 * Integration test: a real server (in a thread of this process, so TSAN can
 * see both sides) plus N real clients over loopback, TCP and UDP.
 * Checks convergence of the disciplined clocks, telemetry, malformed-input
 * handling, split writes, and idle expiry.
 */
#include "../include/client_core.h"
#include "../include/metrics.h"
#include "../include/server.h"
#include "../include/utils.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#define NCLIENTS 12
#define ROUNDS 50

static int g_port;
static const char *g_backend = "auto";

static void apply(server_config_t *c) {
    snprintf(c->backend, sizeof c->backend, "%s", g_backend);
    c->drift_max_clients = 1000;
    c->drain_ms = 500;
}

static int dial(int tcp) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(fd >= 0);
    assert(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    return fd;
}

typedef struct { int tcp; uint32_t id; double final_err_us; int ok; } job_t;

static int exchange(client_t *c, int fd, int tcp, frame_buf_t *fb) {
    msg_t req, rep;
    uint8_t frame[FRAME_LEN];
    client_build_request(c, get_time_ns(), &req);
    proto_encode_frame(&req, frame);
    if (send(fd, frame, sizeof frame, 0) != (ssize_t)sizeof frame) return -1;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 2000) <= 0) return -1;
        uint8_t buf[256];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        uint64_t now = get_time_ns();
        if (n <= 0) return -1;
        if (tcp) {
            size_t cap;
            memcpy(fb_space(fb, &cap), buf, (size_t)n);
            fb_commit(fb, (size_t)n);
            while (fb_next(fb, &rep) == 1)
                if (client_handle_reply(c, &rep, now) == 0) return 0;
        } else if (proto_decode_datagram(buf, (size_t)n, &rep) == 0 && client_handle_reply(c, &rep, now) == 0) {
            return 0;
        }
    }
}

static void *client_thread(void *arg) {
    job_t *j = arg;
    int fd = dial(j->tcp);
    client_t c;
    client_init(&c, j->id, 42, 100.0, 200.0, get_time_ns());
    frame_buf_t fb = {0};
    j->ok = 1;
    for (int r = 0; r < ROUNDS; r++) {
        if (exchange(&c, fd, j->tcp, &fb) < 0) { j->ok = 0; break; }
        usleep(20000);
    }
    j->final_err_us = fabs((double)client_true_error_ns(&c, get_time_ns())) / 1e3;
    /* the initial offset was up to 100 ms; the drift 200 ppm */
    close(fd);
    return NULL;
}

typedef struct { server_t *s; } srv_arg_t;
static void *server_thread(void *a) { server_run(((srv_arg_t *)a)->s); return NULL; }

static void run_mode(int tcp) {
    server_config_t cfg = {.tcp = tcp, .port = 0, .idle_sec = 0, .use_shm = 0};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    assert(s);
    g_port = server_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);

    pthread_t th[NCLIENTS];
    job_t jobs[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        jobs[i] = (job_t){tcp, (uint32_t)(i + 1), 0, 0};
        pthread_create(&th[i], NULL, client_thread, &jobs[i]);
    }
    double worst = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        pthread_join(th[i], NULL);
        assert(jobs[i].ok);
        if (jobs[i].final_err_us > worst) worst = jobs[i].final_err_us;
    }
    /* 1 s of exchanges at 20 ms: converged from a 100 ms start to a few ms at worst; the bound is
     * loose because shared CI runners add scheduling noise */
    assert(worst < 3000.0);

    server_stats_t stats;
    server_get_stats(s, &stats);
    assert(stats.requests == (uint64_t)NCLIENTS * ROUNDS);
    assert(stats.proto_errors == 0);
    metrics_shm_t m;
    metrics_snapshot(server_metrics(s), &m);
    assert(m.requests == stats.requests);
    assert(m.latency.n > 0 && m.latency.mean > 0);
    printf("  %s: %d clients x %d rounds, worst final error %.1f us, %llu requests, mean one-way latency %.1f us\n",
           tcp ? "TCP" : "UDP", NCLIENTS, ROUNDS, worst, (unsigned long long)stats.requests, m.latency.mean / 1e3);

    server_stop(s);
    pthread_join(st, NULL);
    server_destroy(s);
}

/* Frames split across arbitrary writes, back-to-back frames, garbage. */
static void test_tcp_framing_on_wire(void) {
    server_config_t cfg = {.tcp = 1, .port = 0, .idle_sec = 0, .use_shm = 0};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    g_port = server_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);

    int fd = dial(1);
    msg_t m = {.type = MSG_REQUEST, .client_id = 9, .t1 = 1000};
    uint8_t two[2 * FRAME_LEN];
    m.seq = 1; proto_encode_frame(&m, two);
    m.seq = 2; proto_encode_frame(&m, two + FRAME_LEN);
    for (size_t i = 0; i < sizeof two; i++) { /* byte at a time */
        assert(send(fd, two + i, 1, 0) == 1);
        usleep(200);
    }
    uint8_t buf[2 * FRAME_LEN];
    size_t got = 0;
    while (got < sizeof buf) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        assert(poll(&p, 1, 2000) > 0);
        ssize_t n = recv(fd, buf + got, sizeof buf - got, 0);
        assert(n > 0);
        got += (size_t)n;
    }
    msg_t r1, r2;
    frame_buf_t fb = {0};
    memcpy(fb.buf, buf, sizeof buf); fb.len = sizeof buf;
    assert(fb_next(&fb, &r1) == 1 && fb_next(&fb, &r2) == 1);
    assert(r1.type == MSG_REPLY && r1.seq == 1 && r2.seq == 2 && r1.t1 == 1000);
    assert(r1.t2 > 0 && r1.t3 >= r1.t2);

    /* garbage length: server must drop only this connection and stay up */
    uint8_t junk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4};
    assert(send(fd, junk, sizeof junk, 0) == (ssize_t)sizeof junk);
    uint8_t tmp[16];
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 2000) > 0);
    assert(recv(fd, tmp, sizeof tmp, 0) == 0); /* closed by server */
    close(fd);

    int fd2 = dial(1);
    client_t c;
    client_init(&c, 5, 1, 10, 10, get_time_ns());
    frame_buf_t fb2 = {0};
    assert(exchange(&c, fd2, 1, &fb2) == 0); /* still serving */
    close(fd2);

    server_stats_t stats;
    server_get_stats(s, &stats);
    assert(stats.proto_errors == 1);
    printf("  TCP framing: byte-at-a-time requests answered, garbage prefix dropped one connection only\n");
    server_stop(s);
    pthread_join(st, NULL);
    server_destroy(s);
}

static void test_expiry(int tcp) {
    server_config_t cfg = {.tcp = tcp, .port = 0, .idle_sec = 1, .use_shm = 0};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    g_port = server_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);
    int fd = dial(tcp);
    client_t c;
    client_init(&c, 1, 1, 10, 10, get_time_ns());
    frame_buf_t fb = {0};
    assert(exchange(&c, fd, tcp, &fb) == 0);
    server_stats_t stats;
    server_get_stats(s, &stats);
    assert(stats.active == 1);
    for (int i = 0; i < 40; i++) { /* up to 4 s for a 1 s idle timeout */
        usleep(100000);
        server_get_stats(s, &stats);
        if (stats.expired) break;
    }
    assert(stats.expired == 1 && stats.active == 0);
    printf("  %s client expired after idling (active=%u, expired=%llu)\n", tcp ? "TCP" : "UDP", stats.active,
           (unsigned long long)stats.expired);
    close(fd);
    server_stop(s);
    pthread_join(st, NULL);
    server_destroy(s);
}

static void test_max_clients_and_many_fds(void) {
    /* more connections than FD_SETSIZE (1024) on a single poll() loop */
    uint64_t lim = raise_nofile_limit();
    int n = lim > 2600 ? 1500 : (int)(lim / 2) - 50;
    server_config_t cfg = {.tcp = 1, .port = 0, .idle_sec = 0, .use_shm = 0, .backlog = 2048};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    g_port = server_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);
    int *fds = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) fds[i] = dial(1);
    for (int i = 0; i < n; i += 100) { /* every 100th client does a real exchange */
        client_t c;
        client_init(&c, (uint32_t)i, 3, 10, 10, get_time_ns());
        frame_buf_t fb = {0};
        assert(exchange(&c, fds[i], 1, &fb) == 0);
    }
    server_stats_t stats;
    for (int i = 0; i < 50; i++) { server_get_stats(s, &stats); if (stats.active == (uint32_t)n) break; usleep(20000); }
    assert(stats.active == (uint32_t)n);
    printf("  %d concurrent TCP clients on one server (fd limit %llu)\n", n, (unsigned long long)lim);
    for (int i = 0; i < n; i++) close(fds[i]);
    free(fds);
    server_stop(s);
    pthread_join(st, NULL);
    server_destroy(s);
}

/* Minimal HTTP/1.0 GET; returns malloc'd response (headers + body). */
static char *http_get(int port, const char *path) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    char req[256];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\n\r\n", path);
    assert(send(fd, req, (size_t)n, 0) == n);
    char *buf = malloc(1 << 17);
    size_t got = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 3000) <= 0) break;
        ssize_t r = recv(fd, buf + got, (1 << 17) - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    close(fd);
    return buf;
}

typedef struct { uint32_t id; double drift; int ok; } dj_t;

static void *drift_client(void *arg) {
    dj_t *j = arg;
    int fd = dial(1);
    client_t c;
    client_init(&c, j->id, 1000 + j->id, 50.0, 200.0, get_time_ns());
    j->drift = c.hw.drift_ppm;
    frame_buf_t fb = {0};
    j->ok = 1;
    for (int r = 0; r < 200; r++) { /* ~6 s at 30 ms: a longer baseline keeps scheduler jitter on shared CI runners under the bound */
        if (exchange(&c, fd, 1, &fb) < 0) { j->ok = 0; break; }
        usleep(30000);
    }
    close(fd);
    return NULL;
}

/* Sanitizers (TSAN especially) slow every exchange 5-15x, so the timing-derived drift estimate
 * is only checked loosely there; the strict bound applies to normal builds. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define DRIFT_TOL_PPM 100.0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define DRIFT_TOL_PPM 100.0
#endif
#endif
#ifndef DRIFT_TOL_PPM
#define DRIFT_TOL_PPM 30.0
#endif

/* Metrics endpoint and per-client drift, against the true drift of simulated clocks. */
static void test_metrics_and_drift(void) {
    server_config_t cfg = {.tcp = 1, .port = 0, .idle_sec = 0, .use_shm = 0, .http_enabled = 1, .http_port = 0, .drift_top = 10};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    assert(s && server_http_port(s) > 0);
    g_port = server_port(s);
    int hp = server_http_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);

    char *r = http_get(hp, "/healthz");
    assert(strstr(r, "200 OK") && strstr(r, "ok\n"));
    free(r);
    r = http_get(hp, "/nope");
    assert(strstr(r, "404"));
    free(r);

    enum { N = 6 };
    pthread_t th[N];
    dj_t jobs[N];
    for (int i = 0; i < N; i++) { jobs[i].id = (uint32_t)i + 1; pthread_create(&th[i], NULL, drift_client, &jobs[i]); }
    for (int i = 0; i < N; i++) { pthread_join(th[i], NULL); assert(jobs[i].ok); }

    r = http_get(hp, "/metrics");
    assert(strstr(r, "200 OK") && strstr(r, "text/plain"));
    assert(strstr(r, "timesyncd_requests_total 1200"));
    assert(strstr(r, "timesyncd_rtt_seconds_bucket{le=\"+Inf\"}"));
    assert(strstr(r, "timesyncd_drift_ready_clients 6"));
    free(r);

    r = http_get(hp, "/metrics.json");
    assert(strstr(r, "application/json") && strstr(r, "\"requests\":1200"));
    double worst = 0;
    for (int i = 0; i < N; i++) {
        char key[40];
        snprintf(key, sizeof key, "{\"client_id\":%u,\"drift_ppm\":", jobs[i].id);
        char *p = strstr(r, key);
        assert(p);
        double measured = atof(p + strlen(key));
        double err = fabs(measured - jobs[i].drift);
        if (err > worst) worst = err;
        assert(err < DRIFT_TOL_PPM);
    }
    printf("  drift: %d clients with true drift up to 200 ppm, worst server-side estimate error %.1f ppm (loopback)\n", N, worst);
    free(r);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    server_stop(s);
    pthread_join(st, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    assert(ms < 1000);
    /* graceful shutdown closed the listeners */
    struct sockaddr_in sa2 = {0};
    sa2.sin_family = AF_INET;
    sa2.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, "127.0.0.1", &sa2.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(connect(fd, (struct sockaddr *)&sa2, sizeof sa2) < 0);
    close(fd);
    printf("  graceful stop: server_run returned in %.0f ms, listener closed\n", ms);
    server_destroy(s);
}

/* A client that never reads: replies queue up, then the server drops it instead of blocking. */
static void test_slow_reader_dropped(void) {
    server_config_t cfg = {.tcp = 1, .port = 0, .idle_sec = 0, .use_shm = 0, .sndbuf = 4096};
    apply(&cfg);
    server_t *s = server_create(&cfg);
    g_port = server_port(s);
    srv_arg_t sa = {s};
    pthread_t st;
    pthread_create(&st, NULL, server_thread, &sa);
    int fd = dial(1);
    int small = 2048;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    msg_t m = {.type = MSG_REQUEST, .client_id = 1, .t1 = 1};
    uint8_t f[FRAME_LEN];
    server_stats_t st0 = {0};
    for (int i = 0; i < 1000; i++) { /* wait until the server has accepted us (up to 10 s) */
        server_get_stats(s, &st0);
        if (st0.active == 1) break;
        usleep(10000);
    }
    /* Without this, a late accept on a loaded runner reads as "already dropped" below. */
    assert(st0.active == 1);
    /* Flood requests without ever reading replies.  Replies fill the kernel buffers, then the
     * server's per-connection queue, and the connection is dropped.  Bounded by a deadline. */
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0;; i++) {
        m.seq = i;
        proto_encode_frame(&m, f);
        if (send(fd, f, sizeof f, MSG_DONTWAIT) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) break; /* reset: the server dropped us */
            usleep(200);
        }
        if ((i & 1023) == 0) {
            server_stats_t st2;
            server_get_stats(s, &st2);
            clock_gettime(CLOCK_MONOTONIC, &tn);
            if ((st2.active == 0 && i) || tn.tv_sec - t0.tv_sec > 10) break;
        }
    }
    for (int i = 0; i < 500; i++) { /* let the server finish removing it (up to 5 s) */
        server_stats_t st3;
        server_get_stats(s, &st3);
        if (st3.active == 0) break;
        usleep(10000);
    }
    server_stats_t stats;
    server_get_stats(s, &stats);
    if (stats.active != 0) fprintf(stderr, "  slow reader still connected after flood (active=%zu)\n", (size_t)stats.active);
    assert(stats.active == 0);
    printf("  non-reading client dropped after its output queue overflowed\n");
    close(fd);
    server_stop(s);
    pthread_join(st, NULL);
    server_destroy(s);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1) g_backend = argv[1];
    printf("integration tests (backend requested: %s)\n", g_backend);
    run_mode(1);
    run_mode(0);
    test_tcp_framing_on_wire();
    test_expiry(1);
    test_expiry(0);
    test_max_clients_and_many_fds();
    test_slow_reader_dropped();
    test_metrics_and_drift();
    printf("integration tests: passed\n");
    return 0;
}
