// Wire format shared by the Android sender and the PC receiver.
// Authoritative spec: experiment/docs/02-wire-format.md
//
// This header is deliberately dependency-free and C-compatible so the exact
// same file can be compiled into the Android NDK sender and the C++ desktop
// receiver. Two divergent copies of a wire format is how measurement harnesses
// silently produce wrong numbers, so there is only one.

#ifndef UCV_WIRE_H
#define UCV_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- */
/* Magic values                                                      */
/* ---------------------------------------------------------------- */
#define UCV_MAGIC_FRAME   0x55435631u /* "UCV1" */
#define UCV_MAGIC_CONTROL 0x55435643u /* "UCVC" */
#define UCV_MAGIC_ACK     0x55435641u /* "UCVA" */

#define UCV_WIRE_VERSION 1
#define UCV_FRAME_VERSION 2

/* Frame preamble flags */
#define UCV_FLAG_KEYFRAME      0x01
#define UCV_FLAG_FRAGMENTED    0x02
#define UCV_FLAG_LAST_FRAGMENT 0x04

/* Control flags */
#define UCV_CTL_ACK_REQUESTED 0x0001
/* ACK flags */
#define UCV_ACK_APPLIED 0x0001

/* Sizes are contractual — asserted at compile time below. */
#define UCV_FRAME_HEADER_SIZE   56
#define UCV_CONTROL_SIZE        32
#define UCV_ACK_SIZE            40

/* UDP fragment payload. See wire-format spec §1.1 for why 1200 and not the
 * theoretical 1424: it leaves room for VPN/tunnel encapsulation without
 * triggering IP-layer fragmentation, which would hide per-fragment loss. */
#define UCV_UDP_FRAGMENT_PAYLOAD 1200

/* ---------------------------------------------------------------- */
/* Protocol IDs                                                      */
/* ---------------------------------------------------------------- */
typedef enum {
  UCV_PROTO_WEBRTC = 1,
  UCV_PROTO_SRT    = 2,
  UCV_PROTO_RTSP   = 3,
  UCV_PROTO_RAWUDP = 4,
  UCV_PROTO_MJPEG  = 5,
  UCV_PROTO_RTMPS  = 6,
  UCV_PROTO_HLS    = 7,
  /* RTSP with real signalling. It puts the SAME RTP packets on the wire as
   * UCV_PROTO_RTSP (which is the bare RTP data plane started out-of-band), so
   * receivers parse both identically — but a run must record which one carried
   * it, or the results would credit signalling that never took place. */
  UCV_PROTO_RTSP_SIGNALLED = 8,
} ucv_protocol_t;

/* ---------------------------------------------------------------- */
/* Control command types                                             */
/* ---------------------------------------------------------------- */
typedef enum {
  UCV_CMD_SET_ALPHA  = 0x01,
  UCV_CMD_SET_BETA   = 0x02,
  UCV_CMD_SET_ZOOM   = 0x03,
  UCV_CMD_SET_PRESET = 0x04,
  UCV_CMD_PING       = 0x05,
  UCV_CMD_START_RUN  = 0x06,
  UCV_CMD_STOP_RUN   = 0x07,
  UCV_CMD_SET_RUN_HASH = 0x08,
  UCV_CMD_GET_MODE     = 0x09,
} ucv_cmd_type_t;

/* ---------------------------------------------------------------- */
/* Default ports                                                     */
/* ---------------------------------------------------------------- */
#define UCV_PORT_MJPEG    8181
#define UCV_PORT_CONTROL  8200
#define UCV_PORT_RAWUDP   8201
#define UCV_PORT_SRT      8202
#define UCV_PORT_SIGNAL   8203
#define UCV_PORT_HLS      8204
#define UCV_PORT_RTP      5004
#define UCV_PORT_RTSP     8554
#define UCV_PORT_RTMPS    1935

/* ---------------------------------------------------------------- */
/* Structures                                                        */
/* ---------------------------------------------------------------- */
/* Packed so sizeof() equals the on-wire size. Both targets are
 * little-endian (arm64 / x86-64), so no byte swapping is performed —
 * documented in the spec rather than paid for on every frame. */
#pragma pack(push, 1)

typedef struct {
  uint32_t magic;          /* UCV_MAGIC_FRAME                       */
  uint8_t  version;        /* UCV_WIRE_VERSION                      */
  uint8_t  flags;          /* UCV_FLAG_*                            */
  uint16_t protocol_id;    /* ucv_protocol_t                        */
  uint32_t frame_seq;      /* monotonic, never reset within a run   */
  uint32_t packet_seq;     /* monotonic for every UDP datagram      */
  uint32_t payload_bytes;  /* whole-frame size, excl. this header   */
  uint16_t fragment_index; /* zero based within frame               */
  uint16_t fragment_count; /* one for an unfragmented frame         */
  uint64_t t_capture_ns;   /* phone CLOCK_MONOTONIC                 */
  uint64_t t_encoded_ns;   /* phone CLOCK_MONOTONIC                 */
  uint64_t t_sent_ns;      /* phone CLOCK_MONOTONIC, stamped last   */
  uint32_t run_id_hash;    /* FNV-1a of run_id                      */
  uint32_t header_crc32;   /* CRC-32 over all preceding bytes       */
} ucv_frame_header_t;

typedef struct {
  uint32_t magic;          /* UCV_MAGIC_CONTROL                     */
  uint8_t  version;
  uint8_t  cmd_type;       /* ucv_cmd_type_t                        */
  uint16_t flags;          /* UCV_CTL_*                             */
  uint32_t cmd_seq;
  uint64_t t_sent_ns;      /* PC CLOCK_MONOTONIC (t1)               */
  uint8_t  payload[8];     /* interpretation depends on cmd_type    */
  uint32_t crc32;          /* CRC-32 over bytes [0..27]             */
} ucv_control_t;

typedef struct {
  uint32_t magic;          /* UCV_MAGIC_ACK                         */
  uint8_t  version;
  uint8_t  cmd_type;       /* echo                                  */
  uint16_t flags;          /* UCV_ACK_*                             */
  uint32_t cmd_seq;        /* echo                                  */
  uint64_t t_cmd_sent_ns;  /* echo of t1                            */
  uint64_t t_recv_ns;      /* phone clock on arrival (t2)           */
  uint64_t t_ack_ns;       /* phone clock before send (t3)          */
  uint32_t crc32;          /* CRC-32 over bytes [0..35]             */
} ucv_ack_t;

typedef struct {
  uint16_t width;
  uint16_t height;
  uint8_t  fps;
  uint8_t  protocol_id;
  uint16_t video_port;
} ucv_start_run_payload_t;

#pragma pack(pop)

/* Wire sizes are a contract with the spec document, not an implementation
 * detail — a compiler that pads these would corrupt every measurement, so
 * fail the build instead. */
#ifdef __cplusplus
static_assert(sizeof(ucv_frame_header_t) == UCV_FRAME_HEADER_SIZE,
              "frame header must be exactly 56 bytes");
static_assert(sizeof(ucv_control_t) == UCV_CONTROL_SIZE,
              "control message must be exactly 32 bytes");
static_assert(sizeof(ucv_ack_t) == UCV_ACK_SIZE,
              "ack message must be exactly 40 bytes");
static_assert(sizeof(ucv_start_run_payload_t) == 8,
              "start payload must be exactly 8 bytes");
#else
_Static_assert(sizeof(ucv_frame_header_t) == UCV_FRAME_HEADER_SIZE,
               "frame header must be exactly 56 bytes");
_Static_assert(sizeof(ucv_control_t) == UCV_CONTROL_SIZE,
               "control message must be exactly 32 bytes");
_Static_assert(sizeof(ucv_ack_t) == UCV_ACK_SIZE,
               "ack message must be exactly 40 bytes");
_Static_assert(sizeof(ucv_start_run_payload_t) == 8,
               "start payload must be exactly 8 bytes");
#endif

/* ---------------------------------------------------------------- */
/* CRC-32 (IEEE 802.3, reflected) — table-free bitwise form.         */
/* Runs over 44 bytes per frame; at 30 fps that is negligible, and   */
/* avoiding a 1 KiB table keeps this header self-contained.          */
/* ---------------------------------------------------------------- */
static inline uint32_t ucv_crc32(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

/* FNV-1a over the run_id string. Used only to detect a stale sender from a
 * previous run leaking into the current one — not a security mechanism. */
static inline uint32_t ucv_run_id_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619u;
  }
  return h;
}

/* ---------------------------------------------------------------- */
/* Header helpers                                                    */
/* ---------------------------------------------------------------- */

static inline void ucv_frame_header_finalize(ucv_frame_header_t *h) {
  h->magic   = UCV_MAGIC_FRAME;
  h->version = UCV_FRAME_VERSION;
  h->header_crc32 = ucv_crc32(h, UCV_FRAME_HEADER_SIZE - 4);
}

/* Returns 1 if the header is structurally valid. Callers must reject rather
 * than repair: a corrupt frame_seq would poison loss statistics far more than
 * a discarded frame does. */
static inline int ucv_frame_header_valid(const ucv_frame_header_t *h) {
  if (h->magic != UCV_MAGIC_FRAME) return 0;
  if (h->version != UCV_FRAME_VERSION) return 0;
  return h->header_crc32 == ucv_crc32(h, UCV_FRAME_HEADER_SIZE - 4);
}

static inline void ucv_control_finalize(ucv_control_t *c) {
  c->magic   = UCV_MAGIC_CONTROL;
  c->version = UCV_WIRE_VERSION;
  c->crc32   = ucv_crc32(c, UCV_CONTROL_SIZE - 4);
}

static inline int ucv_control_valid(const ucv_control_t *c) {
  if (c->magic != UCV_MAGIC_CONTROL) return 0;
  if (c->version != UCV_WIRE_VERSION) return 0;
  return c->crc32 == ucv_crc32(c, UCV_CONTROL_SIZE - 4);
}

static inline void ucv_ack_finalize(ucv_ack_t *a) {
  a->magic   = UCV_MAGIC_ACK;
  a->version = UCV_WIRE_VERSION;
  a->crc32   = ucv_crc32(a, UCV_ACK_SIZE - 4);
}

static inline int ucv_ack_valid(const ucv_ack_t *a) {
  if (a->magic != UCV_MAGIC_ACK) return 0;
  if (a->version != UCV_WIRE_VERSION) return 0;
  return a->crc32 == ucv_crc32(a, UCV_ACK_SIZE - 4);
}

/* ---------------------------------------------------------------- */
/* Clock offset estimation (harness spec §4.1)                       */
/* ---------------------------------------------------------------- */
/* offset = phone_clock - pc_clock, in nanoseconds.
 * Add it to a PC timestamp to express it on the phone's timebase, or
 * subtract from a phone timestamp to express it on the PC's.
 *
 * Assumes a symmetric path; residual error is roughly half the path
 * asymmetry. See harness spec §4.2 — this is why differences below ~5 ms
 * are reported as unresolvable rather than ranked. */
static inline int64_t ucv_clock_offset_ns(uint64_t t1_pc, uint64_t t2_phone,
                                          uint64_t t3_phone, uint64_t t4_pc) {
  int64_t d1 = (int64_t)t2_phone - (int64_t)t1_pc;
  int64_t d2 = (int64_t)t3_phone - (int64_t)t4_pc;
  return (d1 + d2) / 2;
}

static inline int64_t ucv_round_trip_ns(uint64_t t1_pc, uint64_t t2_phone,
                                        uint64_t t3_phone, uint64_t t4_pc) {
  return ((int64_t)t4_pc - (int64_t)t1_pc) -
         ((int64_t)t3_phone - (int64_t)t2_phone);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UCV_WIRE_H */
