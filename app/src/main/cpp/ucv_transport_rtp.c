/* RTP/UDP H.264 transport (protocol #3 data plane).
 *
 * Packetisation follows RFC 6184: one NAL unit per packet when it fits and
 * FU-A for larger NAL units. Per-frame measurement metadata travels in an
 * RFC 8285 two-byte RTP header extension (profile 0x1000, element id 1).
 * RTSP signalling is deliberately not claimed here; the experiment receiver
 * starts this RTP data plane through the shared control channel.
 */

#include "ucv_transport.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "ucv-rtp"
#define RLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define RLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define RTP_PAYLOAD_TYPE 96
#define RTP_MAX_H264_PAYLOAD 1100
#define RTP_EXTENSION_BYTES 64 /* 4-byte prefix + 60-byte padded body */
#define RTP_PACKET_PREFIX (12 + RTP_EXTENSION_BYTES)

typedef struct {
  int sock;
  struct sockaddr_in peer;
  uint16_t rtp_seq;
  uint32_t packet_seq;
  uint32_t ssrc;
  int started;
} rtp_impl_t;

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int parse_peer(const char *cfg, char *host, size_t host_n, int *port) {
  const char *colon = cfg ? strrchr(cfg, ':') : NULL;
  if (!colon || colon == cfg || (size_t)(colon - cfg) >= host_n) return -EINVAL;
  memcpy(host, cfg, (size_t)(colon - cfg));
  host[colon - cfg] = '\0';
  *port = atoi(colon + 1);
  return *port > 0 && *port <= 65535 ? 0 : -EINVAL;
}

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end,
                                      size_t *size) {
  for (; p + 3 <= end; ++p) {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
      *size = 3; return p;
    }
    if (p + 4 <= end && p[0] == 0 && p[1] == 0 &&
        p[2] == 0 && p[3] == 1) {
      *size = 4; return p;
    }
  }
  return NULL;
}

static uint16_t count_packets(const uint8_t *data, size_t size) {
  const uint8_t *end = data + size;
  size_t sc = 0;
  const uint8_t *start = find_start_code(data, end, &sc);
  if (!start) {
    if (!size) return 0;
    return (uint16_t)(size <= RTP_MAX_H264_PAYLOAD ? 1 :
        (size - 1 + (RTP_MAX_H264_PAYLOAD - 3)) /
            (RTP_MAX_H264_PAYLOAD - 2));
  }
  uint32_t count = 0;
  while (start) {
    const uint8_t *nal = start + sc;
    size_t next_sc = 0;
    const uint8_t *next = find_start_code(nal, end, &next_sc);
    const uint8_t *nal_end = next ? next : end;
    if (nal_end > nal) {
      const size_t n = (size_t)(nal_end - nal);
      count += n <= RTP_MAX_H264_PAYLOAD ? 1 :
          (uint32_t)((n - 1 + (RTP_MAX_H264_PAYLOAD - 3)) /
                     (RTP_MAX_H264_PAYLOAD - 2));
    }
    start = next;
    sc = next_sc;
  }
  return count <= 65535 ? (uint16_t)count : 0;
}

static int send_rtp_packet(rtp_impl_t *im, ucv_transport_t *self,
                           const ucv_encoded_frame_t *f,
                           const uint8_t *payload, size_t payload_size,
                           uint16_t fragment_index, uint16_t fragment_count,
                           int marker) {
  uint8_t packet[RTP_PACKET_PREFIX + RTP_MAX_H264_PAYLOAD];
  packet[0] = 0x90; /* RTP v2 + extension */
  packet[1] = (uint8_t)(RTP_PAYLOAD_TYPE | (marker ? 0x80 : 0));
  put_be16(packet + 2, im->rtp_seq++);
  put_be32(packet + 4, (uint32_t)((f->t_capture_ns * 9ull) / 100000ull));
  put_be32(packet + 8, im->ssrc);
  put_be16(packet + 12, 0x1000); /* RFC 8285 two-byte extension */
  put_be16(packet + 14, 15);     /* 60-byte extension body */
  packet[16] = 1;                /* local extension id */
  packet[17] = UCV_FRAME_HEADER_SIZE;

  ucv_frame_header_t h;
  uint8_t flags = f->is_keyframe ? UCV_FLAG_KEYFRAME : 0;
  if (fragment_count > 1) flags |= UCV_FLAG_FRAGMENTED;
  if (fragment_index + 1 == fragment_count) flags |= UCV_FLAG_LAST_FRAGMENT;
  ucv_fill_header(&h, f, UCV_PROTO_RTSP, (uint32_t)f->size, flags);
  h.packet_seq = im->packet_seq++;
  h.fragment_index = fragment_index;
  h.fragment_count = fragment_count;
  h.t_sent_ns = ucv_now_ns();
  ucv_frame_header_finalize(&h);
  memcpy(packet + 18, &h, sizeof(h));
  packet[74] = packet[75] = 0; /* extension padding */
  memcpy(packet + RTP_PACKET_PREFIX, payload, payload_size);

  const size_t bytes = RTP_PACKET_PREFIX + payload_size;
  const ssize_t sent = sendto(im->sock, packet, bytes, 0,
      (struct sockaddr *)&im->peer, sizeof(im->peer));
  if (sent != (ssize_t)bytes) return -errno;
  self->bytes_wire += (uint64_t)sent;
  return 0;
}

static int send_nal(rtp_impl_t *im, ucv_transport_t *self,
                    const ucv_encoded_frame_t *f,
                    const uint8_t *nal, size_t size,
                    uint16_t *index, uint16_t count) {
  if (!size) return 0;
  if (size <= RTP_MAX_H264_PAYLOAD) {
    const int rc = send_rtp_packet(im, self, f, nal, size, *index, count,
                                   *index + 1 == count);
    (*index)++;
    return rc;
  }
  const uint8_t indicator = (uint8_t)((nal[0] & 0xe0) | 28);
  const uint8_t type = (uint8_t)(nal[0] & 0x1f);
  size_t offset = 1;
  while (offset < size) {
    uint8_t payload[RTP_MAX_H264_PAYLOAD];
    size_t chunk = size - offset;
    if (chunk > RTP_MAX_H264_PAYLOAD - 2) chunk = RTP_MAX_H264_PAYLOAD - 2;
    payload[0] = indicator;
    payload[1] = type;
    if (offset == 1) payload[1] |= 0x80;
    if (offset + chunk == size) payload[1] |= 0x40;
    memcpy(payload + 2, nal + offset, chunk);
    const int rc = send_rtp_packet(im, self, f, payload, chunk + 2,
                                   *index, count, *index + 1 == count);
    (*index)++;
    if (rc < 0) return rc;
    offset += chunk;
  }
  return 0;
}

static int rtp_start(ucv_transport_t *self, const char *cfg) {
  rtp_impl_t *im = (rtp_impl_t *)self->impl;
  if (im->started) return 0;
  char host[48]; int port = 0;
  int rc = parse_peer(cfg, host, sizeof(host), &port);
  if (rc < 0) return rc;
  im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (im->sock < 0) return -errno;
  int sndbuf = 4 * 1024 * 1024;
  setsockopt(im->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  memset(&im->peer, 0, sizeof(im->peer));
  im->peer.sin_family = AF_INET;
  im->peer.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &im->peer.sin_addr) != 1) {
    close(im->sock); im->sock = -1; return -EINVAL;
  }
  im->rtp_seq = (uint16_t)ucv_now_ns();
  im->packet_seq = 0;
  im->ssrc = (uint32_t)(ucv_now_ns() ^ ucv_get_run_id_hash());
  im->started = 1;
  RLOGI("rtp: sending H.264 to %s:%d ssrc=%u", host, port, im->ssrc);
  return 0;
}

static int rtp_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  rtp_impl_t *im = (rtp_impl_t *)self->impl;
  if (!im->started) return -ENOTCONN;
  const uint16_t count = count_packets(f->data, f->size);
  if (!count) return -EINVAL;
  uint16_t index = 0;
  const uint8_t *end = f->data + f->size;
  size_t sc = 0;
  const uint8_t *start = find_start_code(f->data, end, &sc);
  int rc = 0;
  if (!start) {
    rc = send_nal(im, self, f, f->data, f->size, &index, count);
  } else {
    while (start && rc == 0) {
      const uint8_t *nal = start + sc;
      size_t next_sc = 0;
      const uint8_t *next = find_start_code(nal, end, &next_sc);
      const uint8_t *nal_end = next ? next : end;
      if (nal_end > nal)
        rc = send_nal(im, self, f, nal, (size_t)(nal_end - nal), &index, count);
      start = next; sc = next_sc;
    }
  }
  if (rc < 0) { self->send_errors++; return rc; }
  self->frames_sent++;
  self->bytes_payload += f->size;
  return 0;
}

static void rtp_stop(ucv_transport_t *self) {
  rtp_impl_t *im = (rtp_impl_t *)self->impl;
  if (im->sock >= 0) close(im->sock);
  im->sock = -1; im->started = 0;
}

static rtp_impl_t g_rtp_impl = {.sock = -1};
static ucv_transport_t g_rtp = {
    .name = "rtp_udp", .protocol_id = UCV_PROTO_RTSP,
    .start = rtp_start, .send = rtp_send, .stop = rtp_stop,
    .impl = &g_rtp_impl,
};

ucv_transport_t *ucv_transport_rtsp(void) { return &g_rtp; }
ucv_transport_t *ucv_transport_rtp(void) { return &g_rtp; }
