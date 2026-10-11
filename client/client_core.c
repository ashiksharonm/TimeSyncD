#include "../include/client_core.h"
#include <string.h>

static uint64_t next_rand(uint64_t *s) { /* xorshift64* */
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

static double uniform_pm(uint64_t *s) { /* [-1, 1) */
    return (double)(next_rand(s) >> 11) / (double)(1ULL << 52) - 1.0;
}

void client_init(client_t *c, uint32_t id, uint64_t seed, double skew_ms, double drift_ppm,
                 uint64_t real_now_ns) {
    memset(c, 0, sizeof(*c));
    uint64_t s = seed * 0x9E3779B97F4A7C15ULL + id + 1;
    if (!s) s = 1;
    c->id = id;
    c->hw.offset_ns = (int64_t)(uniform_pm(&s) * skew_ms * 1e6);
    c->hw.drift_ppm = uniform_pm(&s) * drift_ppm;
    c->hw.start_ns = real_now_ns;
    disc_init(&c->disc);
}

uint64_t client_time(const client_t *c, uint64_t real_now_ns) {
    return disc_now(&c->disc, simclock_read(&c->hw, real_now_ns));
}

int64_t client_true_error_ns(const client_t *c, uint64_t real_now_ns) {
    return (int64_t)(client_time(c, real_now_ns) - real_now_ns);
}

void client_build_request(client_t *c, uint64_t real_now_ns, msg_t *out) {
    if (c->pending)
        c->dropped++;
    memset(out, 0, sizeof(*out));
    out->type = MSG_REQUEST;
    out->client_id = c->id;
    out->seq = ++c->seq;
    out->t1 = simclock_read(&c->hw, real_now_ns); /* raw hardware time, not disciplined */
    out->last_delay_ns = c->last_delay_ns;
    c->pending = 1;
    c->pending_t1 = out->t1;
}

int client_handle_reply(client_t *c, const msg_t *rep, uint64_t real_now_ns) {
    if (!c->pending || rep->type != MSG_REPLY || rep->client_id != c->id || rep->seq != c->seq ||
        rep->t1 != c->pending_t1)
        return -1;
    uint64_t t4 = simclock_read(&c->hw, real_now_ns);
    int64_t delay = ntp_delay(rep->t1, rep->t2, rep->t3, t4);
    if (delay < 0) /* clock went backwards or corrupt timestamps */
        return -1;
    int64_t offset = ntp_offset(rep->t1, rep->t2, rep->t3, t4);
    c->pending = 0;
    c->replies++;
    c->last_delay_ns = (uint64_t)delay;
    c->last_offset_ns = offset;
    welford_add(&c->delay, (double)delay);
    disc_add(&c->disc, rep->t1 / 2 + t4 / 2, offset, delay);
    return 0;
}
