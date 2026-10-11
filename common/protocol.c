#include "../include/protocol.h"
#include <string.h>

void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

void proto_encode_frame(const msg_t *m, uint8_t out[FRAME_LEN]) {
    put_be32(out, PAYLOAD_LEN);
    uint8_t *p = out + 4;
    p[0] = m->type;
    p[1] = PROTO_VERSION;
    p[2] = p[3] = 0;
    put_be32(p + 4, m->client_id);
    put_be32(p + 8, m->seq);
    put_be64(p + 12, m->t1);
    put_be64(p + 20, m->t2);
    put_be64(p + 28, m->t3);
    put_be64(p + 36, m->last_delay_ns);
}

static int decode_payload(const uint8_t *p, msg_t *m) {
    if (p[1] != PROTO_VERSION || (p[0] != MSG_REQUEST && p[0] != MSG_REPLY))
        return -1;
    m->type = p[0];
    m->client_id = get_be32(p + 4);
    m->seq = get_be32(p + 8);
    m->t1 = get_be64(p + 12);
    m->t2 = get_be64(p + 20);
    m->t3 = get_be64(p + 28);
    m->last_delay_ns = get_be64(p + 36);
    return 0;
}

uint8_t *fb_space(frame_buf_t *fb, size_t *cap) {
    *cap = sizeof(fb->buf) - fb->len;
    return fb->buf + fb->len;
}

void fb_commit(frame_buf_t *fb, size_t n) {
    fb->len += n;
}

int fb_next(frame_buf_t *fb, msg_t *out) {
    if (fb->len < 4)
        return 0;
    uint32_t plen = get_be32(fb->buf);
    if (plen < PAYLOAD_LEN || plen > MAX_PAYLOAD)
        return -1;
    size_t total = 4 + (size_t)plen;
    if (fb->len < total)
        return 0;
    int rc = decode_payload(fb->buf + 4, out);
    if (rc < 0)
        return -1;
    memmove(fb->buf, fb->buf + total, fb->len - total);
    fb->len -= total;
    return 1;
}

int proto_decode_datagram(const uint8_t *data, size_t len, msg_t *out) {
    if (len < 4)
        return -1;
    uint32_t plen = get_be32(data);
    if (plen < PAYLOAD_LEN || plen > MAX_PAYLOAD || len != 4 + (size_t)plen)
        return -1;
    return decode_payload(data + 4, out);
}
