/* Unit tests: Welford, wire format + framing, NTP offset/delay math, discipline. */
#include "../include/drift.h"
#include "../include/ntp.h"
#include "../include/protocol.h"
#include "../include/server.h"
#include "../include/stats.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static msg_t sample_msg(void) {
    msg_t m = {MSG_REPLY, 0xA1B2C3D4u, 77, 0x0102030405060708ULL, 0x1112131415161718ULL,
               0x2122232425262728ULL, 123456789ULL};
    return m;
}

static int test_welford(void) {
    double xs[] = {2, 4, 4, 4, 5, 5, 7, 9, 1e9, 1e9 + 4};
    size_t n = sizeof xs / sizeof xs[0];
    welford_t w = {0};
    double sum = 0;
    for (size_t i = 0; i < n; i++) { welford_add(&w, xs[i]); sum += xs[i]; }
    double mean = sum / (double)n, ss = 0;
    for (size_t i = 0; i < n; i++) ss += (xs[i] - mean) * (xs[i] - mean);
    CHECK(w.n == n);
    CHECK(fabs(w.mean - mean) < 1e-3);
    CHECK(fabs(welford_variance(&w) - ss / (double)(n - 1)) / (ss / (double)(n - 1)) < 1e-9);
    welford_t one = {0};
    welford_add(&one, 5);
    CHECK(welford_variance(&one) == 0.0);
    /* constant series has zero variance even with a huge offset */
    welford_t c = {0};
    for (int i = 0; i < 1000; i++) welford_add(&c, 1e12);
    CHECK(welford_stddev(&c) < 1e-3);
    return 0;
}

static int test_percentile(void) {
    double v[] = {5, 1, 4, 2, 3};
    CHECK(stats_percentile(v, 5, 50) == 3.0);
    CHECK(stats_percentile(v, 5, 0) == 1.0 && stats_percentile(v, 5, 100) == 5.0);
    CHECK(fabs(stats_percentile(v, 5, 25) - 2.0) < 1e-12);
    CHECK(stats_percentile(v, 0, 50) == 0.0);
    return 0;
}

static int test_wire_format(void) {
    msg_t m = sample_msg();
    uint8_t f[FRAME_LEN];
    proto_encode_frame(&m, f);
    static const uint8_t expect[FRAME_LEN] = {
        0, 0, 0, 44, MSG_REPLY, PROTO_VERSION, 0, 0, 0xA1, 0xB2, 0xC3, 0xD4, 0, 0, 0, 77,
        1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0, 0, 0, 0, 0x07, 0x5B, 0xCD, 0x15};
    CHECK(memcmp(f, expect, FRAME_LEN) == 0); /* big-endian on every host */
    msg_t d;
    CHECK(proto_decode_datagram(f, FRAME_LEN, &d) == 0);
    CHECK(memcmp(&d, &m, sizeof m) == 0 || (d.type == m.type && d.client_id == m.client_id &&
          d.seq == m.seq && d.t1 == m.t1 && d.t2 == m.t2 && d.t3 == m.t3 && d.last_delay_ns == m.last_delay_ns));
    CHECK(proto_decode_datagram(f, FRAME_LEN - 1, &d) < 0);   /* truncated datagram */
    uint8_t bad[FRAME_LEN]; memcpy(bad, f, FRAME_LEN); bad[5] = 9;
    CHECK(proto_decode_datagram(bad, FRAME_LEN, &d) < 0);     /* wrong version */
    return 0;
}

static int feed(frame_buf_t *fb, const uint8_t *p, size_t n) {
    size_t cap;
    uint8_t *dst = fb_space(fb, &cap);
    if (n > cap) return -1;
    memcpy(dst, p, n);
    fb_commit(fb, n);
    return 0;
}

static int test_framing(void) {
    msg_t a = sample_msg(), b = sample_msg(), out;
    b.seq = 78;
    uint8_t stream[2 * FRAME_LEN];
    proto_encode_frame(&a, stream);
    proto_encode_frame(&b, stream + FRAME_LEN);

    /* one byte at a time: nothing until the final byte of each frame */
    frame_buf_t fb = {0};
    int got = 0;
    for (size_t i = 0; i < sizeof stream; i++) {
        CHECK(feed(&fb, stream + i, 1) == 0);
        int rc = fb_next(&fb, &out);
        CHECK(rc >= 0);
        if (rc == 1) { got++; CHECK(out.seq == (got == 1 ? 77u : 78u)); CHECK(i == (got == 1 ? FRAME_LEN - 1 : 2 * FRAME_LEN - 1)); }
    }
    CHECK(got == 2 && fb.len == 0);

    /* two frames plus half of a third in a single read */
    frame_buf_t fb2 = {0};
    uint8_t three[3 * FRAME_LEN];
    proto_encode_frame(&a, three); proto_encode_frame(&b, three + FRAME_LEN); proto_encode_frame(&a, three + 2 * FRAME_LEN);
    CHECK(feed(&fb2, three, 2 * FRAME_LEN + 20) == 0);
    CHECK(fb_next(&fb2, &out) == 1 && out.seq == 77);
    CHECK(fb_next(&fb2, &out) == 1 && out.seq == 78);
    CHECK(fb_next(&fb2, &out) == 0);
    CHECK(feed(&fb2, three + 2 * FRAME_LEN + 20, FRAME_LEN - 20) == 0);
    CHECK(fb_next(&fb2, &out) == 1 && out.seq == 77 && fb2.len == 0);

    /* oversize and undersize length prefixes are protocol errors */
    for (uint32_t bad = 0; bad < 2; bad++) {
        frame_buf_t fb3 = {0};
        uint8_t h[4];
        put_be32(h, bad ? MAX_PAYLOAD + 1 : PAYLOAD_LEN - 1);
        CHECK(feed(&fb3, h, 4) == 0);
        CHECK(fb_next(&fb3, &out) == -1);
    }
    /* a longer payload (future version fields) is accepted; extra bytes are skipped */
    uint8_t longf[4 + 52 + FRAME_LEN];
    proto_encode_frame(&a, longf);
    put_be32(longf, 52);
    memset(longf + 4 + PAYLOAD_LEN, 0xEE, 8);
    proto_encode_frame(&b, longf + 4 + 52);
    frame_buf_t fb4 = {0};
    CHECK(feed(&fb4, longf, sizeof longf) == 0);
    CHECK(fb_next(&fb4, &out) == 1 && out.seq == 77);
    CHECK(fb_next(&fb4, &out) == 1 && out.seq == 78);
    return 0;
}

static int test_ntp_math(void) {
    /* client clock is 5 ms behind the server; 1 ms each way; 0.2 ms server processing */
    int64_t off = 5000000, one_way = 1000000, proc = 200000;
    uint64_t t1 = 10000000000ULL;
    uint64_t t2 = t1 + off + one_way, t3 = t2 + proc, t4 = t3 - off + one_way;
    CHECK(ntp_offset(t1, t2, t3, t4) == off);
    CHECK(ntp_delay(t1, t2, t3, t4) == 2 * one_way);
    /* negative offset */
    t2 = t1 - off + one_way; t3 = t2 + proc; t4 = t3 + off + one_way;
    CHECK(ntp_offset(t1, t2, t3, t4) == -off);
    /* asymmetry A shifts the estimate by A/2: 3 ms out, 1 ms back */
    t2 = t1 + off + 3000000; t3 = t2 + proc; t4 = t3 - off + 1000000;
    CHECK(ntp_offset(t1, t2, t3, t4) == off + 1000000);
    CHECK(ntp_delay(t1, t2, t3, t4) == 4000000);
    return 0;
}

static int test_discipline_converges(void) {
    /* Synthetic exchange against a perfect server: clock starts 80 ms off
     * and runs 150 ppm fast; delays jitter between 100-300 us. */
    simclock_t clk = {-80000000, 150.0, 0};
    disc_t d; disc_init(&d);
    uint64_t real = 1000000000ULL, rng = 12345;
    double worst_late = 0;
    for (int k = 0; k < 120; k++) {
        real += 100000000ULL; /* 100 ms poll */
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t out = 50000 + (rng >> 33) % 100000, back = 50000 + (rng >> 13) % 100000;
        uint64_t t1 = simclock_read(&clk, real);
        uint64_t t2 = real + out, t3 = t2 + 1000, t4 = simclock_read(&clk, t3 + back);
        disc_add(&d, t1 / 2 + t4 / 2, ntp_offset(t1, t2, t3, t4), ntp_delay(t1, t2, t3, t4));
        double err = (double)((int64_t)(disc_now(&d, simclock_read(&clk, real + 50000000ULL)) - (real + 50000000ULL)));
        if (k == 0) CHECK(fabs(err) < 1e6);          /* first sample already steps the phase */
        if (k >= 60 && fabs(err) > worst_late) worst_late = fabs(err);
    }
    CHECK(worst_late < 30000);                        /* < 30 us once converged */
    CHECK(fabs(disc_freq_ppm(&d) - (-150.0)) < 15.0); /* correction rate ~ -drift */
    return 0;
}

/* Server-side drift estimate: a client clock running +150 ppm fast, with asymmetric jitter. */
static int test_drift_estimate(void) {
    drift_table_t *t = drift_create(8);
    CHECK(t);
    uint64_t rng = 99, real = 1000000000ULL, delay_prev = 0;
    int64_t off = 40000000; /* server - client at start: 40 ms */
    for (int k = 0; k < 200; k++, real += 100000000ULL) { /* 20 s at 10 Hz */
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t out = 50000 + (rng >> 33) % 40000, back = 50000 + (rng >> 13) % 40000;
        uint64_t t1 = (uint64_t)((double)real * (1.0 + 150e-6)) - (uint64_t)off; /* client clock */
        uint64_t t2 = real + out;
        CHECK(drift_observe(t, 7, t1, t2, delay_prev, real) == 0);
        delay_prev = out + back + 1000;
        if (k == 3) { drift_entry_t e; CHECK(drift_top(t, &e, 1) == 0); } /* not ready yet */
    }
    drift_entry_t e;
    CHECK(drift_top(t, &e, 1) == 1 && e.client_id == 7 && e.ready);
    CHECK(fabs(e.drift_ppm - 150.0) < 5.0);
    drift_summary_t s;
    drift_summary(t, &s);
    CHECK(s.ready == 1 && fabs(s.abs_p50 - 150.0) < 5.0);
    /* capacity is a hard limit: the 9th distinct client is refused, not evicting anyone */
    for (uint32_t id = 100; id < 107; id++) CHECK(drift_observe(t, id, 1, 2, 0, real) == 0);
    CHECK(drift_count(t) == 8);
    CHECK(drift_observe(t, 999, 1, 2, 0, real) == -1 && drift_dropped(t) == 1);
    CHECK(drift_expire(t, real) == 1 && drift_count(t) == 7); /* only client 7 was last seen before `real` */
    drift_destroy(t);
    return 0;
}

static int test_config(void) {
    server_config_t c;
    server_config_defaults(&c);
    CHECK(c.tcp && c.port == 9000 && c.http_enabled && !strcmp(c.backend, "auto"));
    const char *path = "build/test_unit.conf";
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("# comment\nmode = udp   # trailing comment\nport=4242\nbackend = poll\nmax_clients = 77\n"
          "metrics = off\ndrift_max_clients = 5\n", f);
    fclose(f);
    char err[128];
    CHECK(server_config_load(path, &c, err, sizeof err) == 0);
    CHECK(!c.tcp && c.port == 4242 && !strcmp(c.backend, "poll") && c.max_clients == 77 &&
          !c.http_enabled && c.drift_max_clients == 5);
    f = fopen(path, "w");
    fputs("port = 99999\n", f);
    fclose(f);
    CHECK(server_config_load(path, &c, err, sizeof err) == -1 && strstr(err, ":1:"));
    f = fopen(path, "w");
    fputs("nonsense = 1\n", f);
    fclose(f);
    CHECK(server_config_load(path, &c, err, sizeof err) == -1);
    CHECK(server_config_load("build/does-not-exist.conf", &c, err, sizeof err) == -1);
    CHECK(server_config_set(&c, "backend", "kqueue") == -1);
    remove(path);
    return 0;
}

int main(void) {
    int fails = 0;
    fails += test_welford();
    fails += test_percentile();
    fails += test_wire_format();
    fails += test_framing();
    fails += test_ntp_math();
    fails += test_discipline_converges();
    fails += test_drift_estimate();
    fails += test_config();
    printf("unit tests: %s (%d checks)\n", fails ? "FAILED" : "passed", checks);
    return fails ? 1 : 0;
}
