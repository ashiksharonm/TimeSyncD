/* Single TimeSyncD client: connects, polls the server, disciplines its
 * simulated clock and logs the estimate and the (true) residual error. */
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

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int s) { (void)s; g_running = 0; }

static int send_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int tcp = 1, port = 9000, interval_ms = 500, count = 0;
    double skew_ms = 100, drift_ppm = 200;
    uint64_t seed = (uint64_t)get_time_ns() ^ (uint64_t)getpid();
    uint32_t id = (uint32_t)getpid();
    char ip[64] = "127.0.0.1";
    const char *report = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) tcp = !strcmp(argv[++i], "tcp");
        else if (!strcmp(argv[i], "--server-ip") && i + 1 < argc) strncpy(ip, argv[++i], sizeof ip - 1);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skew-ms") && i + 1 < argc) skew_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--drift-ppm") && i + 1 < argc) drift_ppm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--id") && i + 1 < argc) id = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--mode tcp|udp] [--server-ip A] [--port N] [--interval ms] [--count N]\n"
                            "          [--skew-ms N] [--drift-ppm N] [--seed N] [--id N] [--report FILE]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { LOG_ERR("bad address %s", ip); return 1; }
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        LOG_ERR("cannot connect to %s:%d: %s", ip, port, strerror(errno));
        return 1;
    }

    client_t c;
    client_init(&c, id, seed, skew_ms, drift_ppm, get_time_ns());
    LOG_INFO("client %u: %s %s:%d, simulated clock offset %.3f ms, drift %.1f ppm", id,
             tcp ? "TCP" : "UDP", ip, port, (double)c.hw.offset_ns / 1e6, c.hw.drift_ppm);

    frame_buf_t fb = {0};
    FILE *rf = report ? fopen(report, "w") : NULL;
    int done = 0;
    while (g_running && (!count || done < count)) {
        uint64_t tick = get_time_ns();
        msg_t req;
        uint8_t frame[FRAME_LEN];
        client_build_request(&c, tick, &req);
        proto_encode_frame(&req, frame);
        if (send_all(fd, frame, sizeof frame) < 0) { LOG_ERR("send failed: %s", strerror(errno)); break; }

        int got = 0;
        while (!got && g_running) {
            uint64_t waited = (get_time_ns() - tick) / 1000000ULL;
            if (waited >= 1000) break; /* give up on this exchange */
            struct pollfd p = {.fd = fd, .events = POLLIN};
            if (poll(&p, 1, (int)(1000 - waited)) <= 0) continue;
            uint8_t buf[512];
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            uint64_t now = get_time_ns();
            if (n == 0) { LOG_INFO("server closed the connection"); g_running = 0; break; }
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; g_running = 0; break; }
            msg_t rep;
            if (tcp) {
                size_t off = 0;
                while (off < (size_t)n) { /* a read may carry any slice of the stream */
                    size_t cap, take;
                    uint8_t *dst = fb_space(&fb, &cap);
                    take = (size_t)n - off < cap ? (size_t)n - off : cap;
                    memcpy(dst, buf + off, take);
                    fb_commit(&fb, take);
                    off += take;
                    int rc;
                    while ((rc = fb_next(&fb, &rep)) == 1)
                        if (client_handle_reply(&c, &rep, now) == 0) got = 1;
                    if (rc < 0) { LOG_ERR("protocol error from server"); g_running = 0; break; }
                }
            } else if (proto_decode_datagram(buf, (size_t)n, &rep) == 0 && client_handle_reply(&c, &rep, now) == 0) {
                got = 1;
            }
            if (got) {
                int64_t err = client_true_error_ns(&c, now);
                LOG_INFO("offset=%+.1fus delay=%.1fus freq=%+.2fppm | residual error vs true time=%+.1fus (%d samples)",
                         (double)c.last_offset_ns / 1e3, (double)c.last_delay_ns / 1e3, disc_freq_ppm(&c.disc),
                         (double)err / 1e3, c.disc.used);
                if (rf) fprintf(rf, "%llu %lld\n", (unsigned long long)c.replies, (long long)err);
            }
        }
        done++;
        uint64_t spent = (get_time_ns() - tick) / 1000000ULL;
        if (g_running && spent < (uint64_t)interval_ms) usleep((useconds_t)((uint64_t)interval_ms - spent) * 1000);
    }
    if (rf) fclose(rf);
    close(fd);
    LOG_INFO("client %u done: %llu replies, estimated frequency error %+.2f ppm (true %+.2f ppm)", id,
             (unsigned long long)c.replies, -disc_freq_ppm(&c.disc), c.hw.drift_ppm);
    return 0;
}
