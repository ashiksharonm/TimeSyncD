#ifndef CLIENT_CORE_H
#define CLIENT_CORE_H

#include "ntp.h"
#include "protocol.h"
#include "stats.h"

/*
 * One simulated client: a skewed, drifting hardware clock plus the
 * discipline loop that corrects it from NTP-style exchanges with the server.
 * Pure state machine (no sockets) so it can be driven by the CLI client, the
 * load generator and the tests.
 */
typedef struct {
    uint32_t id;
    simclock_t hw;          /* simulated oscillator */
    disc_t disc;
    uint32_t seq;
    int pending;            /* request outstanding */
    uint64_t pending_t1;
    uint64_t last_delay_ns;
    uint64_t replies, dropped;
    int64_t last_offset_ns;
    welford_t delay;
} client_t;

/* skew_ms / drift_ppm are maxima: the initial offset and the frequency error
 * are drawn uniformly from [-max, +max] using seed. */
void client_init(client_t *c, uint32_t id, uint64_t seed, double skew_ms, double drift_ppm,
                 uint64_t real_now_ns);
void client_build_request(client_t *c, uint64_t real_now_ns, msg_t *out);
/* 0 = accepted and applied; -1 = stale/invalid reply ignored. */
int client_handle_reply(client_t *c, const msg_t *rep, uint64_t real_now_ns);
/* Disciplined clock reading at the given real (true) time. */
uint64_t client_time(const client_t *c, uint64_t real_now_ns);
/* Disciplined clock minus true time.  Meaningful only when the server's clock
 * is the same monotonic clock (localhost), which is how the load test uses it. */
int64_t client_true_error_ns(const client_t *c, uint64_t real_now_ns);

#endif
