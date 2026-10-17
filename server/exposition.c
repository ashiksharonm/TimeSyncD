#include "../include/exposition.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct { char *buf; size_t cap, len; } out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...) {
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
}

static void metric(out_t *o, const char *name, const char *type, const char *help, const char *fmt, double v) {
    put(o, "# HELP %s %s\n# TYPE %s %s\n%s ", name, help, name, type, name);
    put(o, fmt, v);
    put(o, "\n");
}

size_t expo_prometheus(char *buf, size_t cap, const expo_t *e) {
    out_t o = {buf, cap, 0};
    if (cap) buf[0] = 0;
    metric(&o, "timesyncd_clients", "gauge", "Clients currently tracked (TCP connections or UDP peers).", "%.0f", e->srv.active);
    metric(&o, "timesyncd_requests_total", "counter", "Time requests answered.", "%.0f", (double)e->srv.requests);
    metric(&o, "timesyncd_connections_accepted_total", "counter", "TCP connections accepted.", "%.0f", (double)e->srv.accepted);
    metric(&o, "timesyncd_connections_rejected_total", "counter", "TCP connections refused at max_clients.", "%.0f", (double)e->srv.rejected);
    metric(&o, "timesyncd_clients_expired_total", "counter", "Idle clients dropped.", "%.0f", (double)e->srv.expired);
    metric(&o, "timesyncd_protocol_errors_total", "counter", "Malformed frames or datagrams.", "%.0f", (double)e->srv.proto_errors);
    metric(&o, "timesyncd_accept_errors_total", "counter", "accept() failures such as EMFILE.", "%.0f", (double)e->srv.accept_errors);
    metric(&o, "timesyncd_uptime_seconds", "gauge", "Seconds since the server started.", "%.1f", e->uptime_s);
    put(&o, "# HELP timesyncd_rtt_seconds Round-trip delay as measured and reported by clients.\n# TYPE timesyncd_rtt_seconds histogram\n");
    uint64_t cum = 0;
    for (int b = 0; b < RTT_BUCKETS; b++) {
        cum += e->m.rtt_hist[b];
        if (b < RTT_BUCKETS - 1) put(&o, "timesyncd_rtt_seconds_bucket{le=\"%g\"} %llu\n", rtt_bucket_us[b] / 1e6, (unsigned long long)cum);
        else put(&o, "timesyncd_rtt_seconds_bucket{le=\"+Inf\"} %llu\n", (unsigned long long)cum);
    }
    put(&o, "timesyncd_rtt_seconds_sum %.9f\ntimesyncd_rtt_seconds_count %llu\n", (double)e->m.rtt_sum_ns / 1e9, (unsigned long long)e->m.rtt_count);
    metric(&o, "timesyncd_drift_tracked_clients", "gauge", "Clients with a drift record.", "%.0f", (double)e->drift_tracked);
    metric(&o, "timesyncd_drift_clients_dropped_total", "counter", "New clients not tracked because the drift table was full.", "%.0f", (double)e->drift_dropped);
    metric(&o, "timesyncd_drift_ready_clients", "gauge", "Clients with enough samples for a drift estimate.", "%.0f", (double)e->drift.ready);
    put(&o, "# HELP timesyncd_drift_abs_ppm Absolute client clock drift across ready clients.\n# TYPE timesyncd_drift_abs_ppm gauge\n");
    put(&o, "timesyncd_drift_abs_ppm{stat=\"p50\"} %.3f\ntimesyncd_drift_abs_ppm{stat=\"p99\"} %.3f\ntimesyncd_drift_abs_ppm{stat=\"max\"} %.3f\n",
        e->drift.abs_p50, e->drift.abs_p99, e->drift.abs_max);
    put(&o, "# HELP timesyncd_info Build and runtime information.\n# TYPE timesyncd_info gauge\ntimesyncd_info{backend=\"%s\",mode=\"%s\"} 1\n",
        e->backend, e->tcp ? "tcp" : "udp");
    return o.len;
}

size_t expo_json(char *buf, size_t cap, const expo_t *e) {
    out_t o = {buf, cap, 0};
    if (cap) buf[0] = 0;
    put(&o, "{\"backend\":\"%s\",\"mode\":\"%s\",\"uptime_s\":%.1f,", e->backend, e->tcp ? "tcp" : "udp", e->uptime_s);
    put(&o, "\"clients\":%u,\"requests\":%llu,\"accepted\":%llu,\"rejected\":%llu,\"expired\":%llu,\"protocol_errors\":%llu,\"accept_errors\":%llu,",
        e->srv.active, (unsigned long long)e->srv.requests, (unsigned long long)e->srv.accepted,
        (unsigned long long)e->srv.rejected, (unsigned long long)e->srv.expired,
        (unsigned long long)e->srv.proto_errors, (unsigned long long)e->srv.accept_errors);
    put(&o, "\"rtt\":{\"count\":%llu,\"mean_us\":%.1f,\"buckets_us\":[", (unsigned long long)e->m.rtt_count,
        e->m.rtt_count ? (double)e->m.rtt_sum_ns / (double)e->m.rtt_count / 1e3 : 0.0);
    for (int b = 0; b < RTT_BUCKETS; b++) {
        if (b < RTT_BUCKETS - 1) put(&o, "%s{\"le\":%u,\"count\":%llu}", b ? "," : "", rtt_bucket_us[b], (unsigned long long)e->m.rtt_hist[b]);
        else put(&o, ",{\"le\":null,\"count\":%llu}", (unsigned long long)e->m.rtt_hist[b]);
    }
    put(&o, "]},\"drift\":{\"tracked\":%zu,\"ready\":%zu,\"dropped\":%llu,\"abs_ppm\":{\"p50\":%.3f,\"p99\":%.3f,\"max\":%.3f},\"top\":[",
        e->drift_tracked, e->drift.ready, (unsigned long long)e->drift_dropped, e->drift.abs_p50, e->drift.abs_p99, e->drift.abs_max);
    for (size_t i = 0; i < e->ntop; i++)
        put(&o, "%s{\"client_id\":%u,\"drift_ppm\":%.3f,\"offset_us\":%.1f,\"samples\":%u,\"span_s\":%.1f}", i ? "," : "",
            e->top[i].client_id, e->top[i].drift_ppm, e->top[i].offset_us, e->top[i].samples, e->top[i].span_s);
    put(&o, "]}}\n");
    return o.len;
}
