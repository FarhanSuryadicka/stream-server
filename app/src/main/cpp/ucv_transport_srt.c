/* SRT transport (protocol #2), phone caller -> PC listener.
 *
 * Encoded H.264 access units use the same instrumented 1200-byte fragments as
 * Raw UDP, but each fragment is one SRT live/message-mode message. SRT itself
 * handles loss detection, retransmission, ordering and delivery latency.
 */

#include "ucv_transport.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <srt.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ucv-srt"
#define SLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define SLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define UCV_SRT_LATENCY_MS 20

typedef struct {
  SRTSOCKET sock;
  uint32_t packet_seq;
  int started;
} srt_impl_t;

static int parse_peer(const char *cfg, char *host, size_t host_n, int *port) {
  const char *colon = cfg ? strrchr(cfg, ':') : NULL;
  if (!colon || colon == cfg || (size_t)(colon - cfg) >= host_n) return -EINVAL;
  memcpy(host, cfg, (size_t)(colon - cfg));
  host[colon - cfg] = '\0';
  *port = atoi(colon + 1);
  return *port > 0 && *port <= 65535 ? 0 : -EINVAL;
}

static int set_flag(SRTSOCKET sock, SRT_SOCKOPT option,
                    const void *value, int size) {
  if (srt_setsockflag(sock, option, value, size) == SRT_ERROR) {
    SLOGE("srt: setsockopt %d failed: %s", (int)option,
          srt_getlasterror_str());
    return -EIO;
  }
  return 0;
}

static int srt_start_transport(ucv_transport_t *self, const char *cfg) {
  srt_impl_t *im = (srt_impl_t *)self->impl;
  if (im->started) return 0;
  char host[48]; int port = 0;
  int rc = parse_peer(cfg, host, sizeof(host), &port);
  if (rc < 0) return rc;
  if (srt_startup() == SRT_ERROR) return -EIO;
  im->sock = srt_create_socket();
  if (im->sock == SRT_INVALID_SOCK) return -EIO;

  SRT_TRANSTYPE type = SRTT_LIVE;
  int yes = 1;
  int latency = UCV_SRT_LATENCY_MS;
  int payload = UCV_FRAME_HEADER_SIZE + UCV_UDP_FRAGMENT_PAYLOAD;
  if (set_flag(im->sock, SRTO_TRANSTYPE, &type, sizeof(type)) < 0 ||
      set_flag(im->sock, SRTO_MESSAGEAPI, &yes, sizeof(yes)) < 0 ||
      set_flag(im->sock, SRTO_TLPKTDROP, &yes, sizeof(yes)) < 0 ||
      set_flag(im->sock, SRTO_LATENCY, &latency, sizeof(latency)) < 0 ||
      set_flag(im->sock, SRTO_PAYLOADSIZE, &payload, sizeof(payload)) < 0) {
    srt_close(im->sock); im->sock = SRT_INVALID_SOCK; return -EIO;
  }

  struct sockaddr_in peer;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &peer.sin_addr) != 1) {
    srt_close(im->sock); im->sock = SRT_INVALID_SOCK; return -EINVAL;
  }
  if (srt_connect(im->sock, (struct sockaddr *)&peer, sizeof(peer)) == SRT_ERROR) {
    SLOGE("srt: connect %s:%d failed: %s", host, port,
          srt_getlasterror_str());
    srt_close(im->sock); im->sock = SRT_INVALID_SOCK; return -ECONNREFUSED;
  }
  im->packet_seq = 0;
  im->started = 1;
  SLOGI("srt: connected to %s:%d latency=%dms encryption=off",
        host, port, latency);
  return 0;
}

static int srt_send_frame(ucv_transport_t *self,
                          const ucv_encoded_frame_t *f) {
  srt_impl_t *im = (srt_impl_t *)self->impl;
  if (!im->started) return -ENOTCONN;
  uint8_t message[UCV_FRAME_HEADER_SIZE + UCV_UDP_FRAGMENT_PAYLOAD];
  size_t offset = 0;
  const uint16_t count = (uint16_t)(
      (f->size + UCV_UDP_FRAGMENT_PAYLOAD - 1) / UCV_UDP_FRAGMENT_PAYLOAD);
  for (uint16_t index = 0; index < count; ++index) {
    size_t chunk = f->size - offset;
    if (chunk > UCV_UDP_FRAGMENT_PAYLOAD) chunk = UCV_UDP_FRAGMENT_PAYLOAD;
    ucv_frame_header_t h;
    uint8_t flags = f->is_keyframe ? UCV_FLAG_KEYFRAME : 0;
    if (count > 1) flags |= UCV_FLAG_FRAGMENTED;
    if (index + 1 == count) flags |= UCV_FLAG_LAST_FRAGMENT;
    ucv_fill_header(&h, f, UCV_PROTO_SRT, (uint32_t)f->size, flags);
    h.packet_seq = im->packet_seq++;
    h.fragment_index = index;
    h.fragment_count = count;
    h.t_sent_ns = ucv_now_ns();
    ucv_frame_header_finalize(&h);
    memcpy(message, &h, sizeof(h));
    memcpy(message + sizeof(h), f->data + offset, chunk);
    const int bytes = (int)(sizeof(h) + chunk);
    if (srt_sendmsg(im->sock, (const char *)message, bytes, -1, 0) != bytes) {
      SLOGE("srt: send failed: %s", srt_getlasterror_str());
      self->send_errors++;
      return -EIO;
    }
    self->bytes_wire += (uint64_t)bytes;
    offset += chunk;
  }
  self->frames_sent++;
  self->bytes_payload += f->size;
  return 0;
}

static void srt_stop_transport(ucv_transport_t *self) {
  srt_impl_t *im = (srt_impl_t *)self->impl;
  if (im->sock != SRT_INVALID_SOCK) srt_close(im->sock);
  im->sock = SRT_INVALID_SOCK;
  im->started = 0;
  srt_cleanup();
}

static srt_impl_t g_srt_impl = {.sock = SRT_INVALID_SOCK};
static ucv_transport_t g_srt = {
    .name = "srt", .protocol_id = UCV_PROTO_SRT,
    .start = srt_start_transport, .send = srt_send_frame,
    .stop = srt_stop_transport, .impl = &g_srt_impl,
};

ucv_transport_t *ucv_transport_srt(void) { return &g_srt; }
