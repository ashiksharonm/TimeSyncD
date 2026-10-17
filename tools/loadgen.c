/*
 * loadgen: N simulated clients in one process, each with its own socket,
 * simulated skewed clock and discipline loop.  Measures how many connect and
 * the converged offset error against true time (valid on localhost because
 * client and server share CLOCK_MONOTONIC).
 *
 * Prints one JSON object.
 */
#include "../include/client_core.h"
#include "../include/utils.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    client_t c;
    int fd;
    int up;
    frame_buf_t fb;
    uint64_t next_send;
} lc_t;

static int dial_nonblocking(int tcp, const struct sockaddr_in *sa, int *err) {
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) { *err = errno; return -1; }
    set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, (const struct sockaddr *)sa, sizeof(*sa)) < 0 && errno != EINPROGRESS) {
        *err = errno; close(fd); return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    int tcp = 1, port = 9000, n = 100, interval_ms = 200, warmup_s = 5, duration_s = 15;
    double skew_ms = 100, drift_ppm = 200;
    char ip[64] = "127.0.0.1";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) tcp = !strcmp(argv[++i], "tcp");
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clients") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skew-ms") && i + 1 < argc) skew_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--drift-ppm") && i + 1 < argc) drift_ppm = atof(argv[++i]);
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    uint64_t nofile = raise_nofile_limit();
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &sa.sin_addr);

    lc_t *cl = calloc((size_t)n, sizeof(lc_t));
    struct pollfd *pf = calloc((size_t)n, sizeof(struct pollfd));
    int *idx = calloc((size_t)n, sizeof(int));
    size_t max_samples = (size_t)n * (size_t)((duration_s * 1000 / interval_ms) + 4);
    double *errs = malloc(max_samples * sizeof(double));
    size_t nerr = 0;
    int fail_emfile = 0, fail_refused = 0, fail_timeout = 0, fail_addr = 0, fail_other = 0, up = 0;

    /* connect in batches so the listen backlog is never overrun */
    uint64_t t_connect0 = get_time_ns();
    for (int base = 0; base < n; base += 100) {
        int end = base + 100 < n ? base + 100 : n, inflight = 0;
        for (int i = base; i < end; i++) {
            int err = 0;
            cl[i].fd = dial_nonblocking(tcp, &sa, &err);
            if (cl[i].fd < 0) {
                if (err == EMFILE || err == ENFILE) fail_emfile++;
                else if (err == ECONNREFUSED) fail_refused++;
                else if (err == EADDRNOTAVAIL) fail_addr++;
                else fail_other++;
            } else {
                inflight++;
            }
        }
        uint64_t t0 = get_time_ns();
        while (inflight && get_time_ns() - t0 < 5000000000ULL) {
            int np = 0;
            for (int i = base; i < end; i++)
                if (cl[i].fd >= 0 && !cl[i].up) { pf[np] = (struct pollfd){.fd = cl[i].fd, .events = POLLOUT}; idx[np++] = i; }
            if (!np) break;
            poll(pf, (nfds_t)np, 100);
            for (int k = 0; k < np; k++) {
                if (!pf[k].revents) continue;
                int e = 0; socklen_t l = sizeof e;
                getsockopt(pf[k].fd, SOL_SOCKET, SO_ERROR, &e, &l);
                lc_t *x = &cl[idx[k]];
                if (e == 0 && !(pf[k].revents & (POLLERR | POLLHUP))) { x->up = 1; up++; }
                else {
                    close(x->fd); x->fd = -1;
                    if (e == ECONNREFUSED) fail_refused++;
                    else if (e == ETIMEDOUT) fail_timeout++;
                    else if (e == EADDRNOTAVAIL) fail_addr++;
                    else fail_other++;
                }
                inflight--;
            }
        }
        for (int i = base; i < end; i++) /* still pending after 5 s */
            if (cl[i].fd >= 0 && !cl[i].up) { close(cl[i].fd); cl[i].fd = -1; fail_timeout++; }
    }
    double connect_s = (double)(get_time_ns() - t_connect0) / 1e9;

    uint64_t start = get_time_ns();
    uint64_t interval_ns = (uint64_t)interval_ms * 1000000ULL;
    for (int i = 0; i < n; i++) {
        client_init(&cl[i].c, (uint32_t)i + 1, 7, skew_ms, drift_ppm, start);
        cl[i].next_send = start + (uint64_t)i * interval_ns / (uint64_t)(n ? n : 1); /* stagger */
    }
    uint64_t warm_end = start + (uint64_t)warmup_s * 1000000000ULL;
    uint64_t end_ns = start + (uint64_t)duration_s * 1000000000ULL;
    uint64_t sent = 0, replies = 0, closed = 0;

    while (get_time_ns() < end_ns) {
        uint64_t now = get_time_ns();
        int np = 0;
        for (int i = 0; i < n; i++) {
            lc_t *x = &cl[i];
            if (!x->up) continue;
            if (now >= x->next_send) {
                if (now >= warm_end && nerr < max_samples) /* error just before the exchange */
                    errs[nerr++] = (double)llabs(client_true_error_ns(&x->c, now));
                msg_t req;
                uint8_t f[FRAME_LEN];
                client_build_request(&x->c, now, &req);
                proto_encode_frame(&req, f);
                if (send(x->fd, f, sizeof f, 0) < 0 && errno != EAGAIN) { close(x->fd); x->up = 0; closed++; continue; }
                sent++;
                x->next_send += interval_ns;
            }
            pf[np] = (struct pollfd){.fd = x->fd, .events = POLLIN};
            idx[np++] = i;
        }
        if (poll(pf, (nfds_t)np, 2) <= 0) continue;
        uint64_t rnow = get_time_ns();
        for (int k = 0; k < np; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            lc_t *x = &cl[idx[k]];
            uint8_t buf[512];
            ssize_t r = recv(x->fd, buf, sizeof buf, 0);
            if (r <= 0) { if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue; close(x->fd); x->up = 0; closed++; continue; }
            msg_t rep;
            if (tcp) {
                size_t cap;
                uint8_t *d = fb_space(&x->fb, &cap);
                if ((size_t)r > cap) { close(x->fd); x->up = 0; closed++; continue; }
                memcpy(d, buf, (size_t)r);
                fb_commit(&x->fb, (size_t)r);
                while (fb_next(&x->fb, &rep) == 1)
                    if (client_handle_reply(&x->c, &rep, rnow) == 0) replies++;
            } else if (proto_decode_datagram(buf, (size_t)r, &rep) == 0 && client_handle_reply(&x->c, &rep, rnow) == 0) {
                replies++;
            }
        }
    }

    welford_t delay = {0};
    for (int i = 0; i < n; i++)
        if (cl[i].c.delay.n) welford_add(&delay, cl[i].c.delay.mean);
    double p50 = stats_percentile(errs, nerr, 50), p95 = stats_percentile(errs, nerr, 95),
           p99 = stats_percentile(errs, nerr, 99), mx = nerr ? errs[nerr - 1] : 0;
    printf("{\"mode\":\"%s\",\"clients_requested\":%d,\"connected\":%d,\"failed_emfile\":%d,\"failed_refused\":%d,\"failed_timeout\":%d,\"failed_addr_not_avail\":%d,"
           "\"failed_other\":%d,\"dropped_after_connect\":%llu,\"connect_seconds\":%.2f,\"nofile_limit\":%llu,"
           "\"interval_ms\":%d,\"warmup_s\":%d,\"duration_s\":%d,\"requests_sent\":%llu,\"replies\":%llu,"
           "\"error_samples\":%zu,\"abs_error_us\":{\"p50\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
           "\"mean_rtt_us\":%.1f}\n",
           tcp ? "tcp" : "udp", n, up, fail_emfile, fail_refused, fail_timeout, fail_addr, fail_other, (unsigned long long)closed,
           connect_s, (unsigned long long)nofile, interval_ms, warmup_s, duration_s, (unsigned long long)sent,
           (unsigned long long)replies, nerr, p50 / 1e3, p95 / 1e3, p99 / 1e3, mx / 1e3, delay.mean / 1e3);
    return 0;
}
