#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wire format (all integers big-endian / network byte order):
 *
 *   frame   = u32 payload_len | payload
 *   payload = u8 type | u8 version | u16 reserved(0) | u32 client_id | u32 seq
 *             | u64 t1 | u64 t2 | u64 t3 | u64 last_delay_ns          (44 bytes)
 *
 * NTP-style exchange: the client stamps t1 (its clock) on a REQUEST; the
 * server stamps t2 (receive) and t3 (send) on the REPLY; the client reads t4
 * on arrival.  Receivers accept payloads up to MAX_PAYLOAD bytes and ignore
 * bytes past the fields they know, so the format can grow.  The same framing
 * is used for TCP streams and UDP datagrams.
 */
#define PROTO_VERSION 1
#define PAYLOAD_LEN 44
#define MAX_PAYLOAD 64
#define FRAME_LEN (4 + PAYLOAD_LEN)

typedef enum { MSG_REQUEST = 1, MSG_REPLY = 2 } msg_type_t;

typedef struct {
    uint8_t type;
    uint32_t client_id;
    uint32_t seq;
    uint64_t t1, t2, t3;
    uint64_t last_delay_ns; /* client's previous measured round-trip delay, 0 if none */
} msg_t;

void put_be32(uint8_t *p, uint32_t v);
void put_be64(uint8_t *p, uint64_t v);
uint32_t get_be32(const uint8_t *p);
uint64_t get_be64(const uint8_t *p);

/* Writes exactly FRAME_LEN bytes. */
void proto_encode_frame(const msg_t *m, uint8_t out[FRAME_LEN]);

/* Reassembles frames from an arbitrary byte stream (partial reads, several
 * frames per read).  Fill via fb_space()/fb_commit(), drain with fb_next(). */
typedef struct {
    uint8_t buf[256];
    size_t len;
} frame_buf_t;

uint8_t *fb_space(frame_buf_t *fb, size_t *cap);
void fb_commit(frame_buf_t *fb, size_t n);
/* 1 = message returned, 0 = need more bytes, -1 = protocol error (close the stream). */
int fb_next(frame_buf_t *fb, msg_t *out);
/* Decode one complete datagram (must hold exactly one frame). 0 ok, -1 invalid. */
int proto_decode_datagram(const uint8_t *data, size_t len, msg_t *out);

#endif
