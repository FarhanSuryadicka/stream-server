/* Shared transport helpers + the transports that need no external library.
 *
 * Raw UDP and MJPEG live here because they are implementable with plain
 * POSIX sockets. SRT / WebRTC / RTSP / RTMPS / HLS each need a vendored
 * library and live in their own files once those are integrated; until then
 * ucv_transport_get() returns NULL for them so a run can never be silently
 * mislabelled as a protocol that did not actually carry the bytes.
 */

#include "ucv_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <android/log.h>

#define TAG "ucv-transport"
#define TLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define TLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ---------------------------------------------------------------- */
/* Shared helpers                                                    */
/* ---------------------------------------------------------------- */

uint64_t ucv_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t g_run_id_hash = 0;

void ucv_set_run_id(const char *run_id) {
  g_run_id_hash = run_id ? ucv_run_id_hash(run_id) : 0;
}

void ucv_set_run_id_hash(uint32_t run_id_hash) { g_run_id_hash = run_id_hash; }

uint32_t ucv_get_run_id_hash(void) { return g_run_id_hash; }

void ucv_fill_header(ucv_frame_header_t *h, const ucv_encoded_frame_t *f,
                     uint16_t protocol_id, uint32_t payload_bytes,
                     uint8_t extra_flags) {
  memset(h, 0, sizeof(*h));
  h->protocol_id   = protocol_id;
  h->frame_seq     = f->seq;
  h->payload_bytes = payload_bytes;
  h->t_capture_ns  = f->t_capture_ns;
  h->t_encoded_ns  = f->t_encoded_ns;
  h->run_id_hash   = g_run_id_hash;
  h->flags         = extra_flags;
  if (f->is_keyframe)
    h->flags |= UCV_FLAG_KEYFRAME;
  /* t_sent_ns and the CRC are set by the transport immediately before the
   * write — see ucv_transport.h. */
}

/* ================================================================ */
/* Raw UDP                                                           */
/* ================================================================ */
/* The latency lower bound and the reliability lower bound at once. No
 * handshake, no encryption, no retransmission — exactly as described in the
 * issue appendix. Frames larger than one datagram are fragmented at the
 * application layer (wire spec §1.1) rather than left to IP fragmentation,
 * so per-fragment loss stays visible in the measurements. */

typedef struct {
  int                sock;
  struct sockaddr_in peer;
  int                started;
  uint32_t           packet_seq;
} rawudp_impl_t;

static int rawudp_start(ucv_transport_t *self, const char *cfg) {
  rawudp_impl_t *im = (rawudp_impl_t *)self->impl;
  if (im->started)
    return 0;

  /* cfg is "ip:port"; port defaults to UCV_PORT_RAWUDP when absent. */
  char host[64] = {0};
  int  port     = UCV_PORT_RAWUDP;
  if (!cfg || !*cfg) {
    TLOGE("rawudp: missing peer address");
    return -EINVAL;
  }
  const char *colon = strrchr(cfg, ':');
  if (colon) {
    size_t n = (size_t)(colon - cfg);
    if (n >= sizeof(host)) n = sizeof(host) - 1;
    memcpy(host, cfg, n);
    port = atoi(colon + 1);
  } else {
    strncpy(host, cfg, sizeof(host) - 1);
  }

  im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (im->sock < 0) {
    TLOGE("rawudp: socket() failed errno=%d", errno);
    return -errno;
  }

  /* A large send buffer keeps a burst of keyframe fragments from being
   * dropped locally before they ever reach the wire — a local drop would be
   * miscounted as network loss and corrupt the reliability metric. */
  int sndbuf = 4 * 1024 * 1024;
  setsockopt(im->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  memset(&im->peer, 0, sizeof(im->peer));
  im->peer.sin_family = AF_INET;
  im->peer.sin_port   = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &im->peer.sin_addr) != 1) {
    TLOGE("rawudp: bad peer address '%s'", host);
    close(im->sock);
    im->sock = -1;
    return -EINVAL;
  }

  im->started = 1;
  im->packet_seq = 0;
  TLOGI("rawudp: sending to %s:%d", host, port);
  return 0;
}

static int rawudp_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  rawudp_impl_t *im = (rawudp_impl_t *)self->impl;
  if (!im->started)
    return -ENOTCONN;

  /* One stack buffer reused for every datagram: header + fragment payload. */
  uint8_t pkt[UCV_FRAME_HEADER_SIZE + UCV_UDP_FRAGMENT_PAYLOAD];
  ucv_frame_header_t *h = (ucv_frame_header_t *)pkt;

  size_t offset    = 0;
  int    fragments = 0;
  int    failed    = 0;
  const uint16_t fragment_count = (uint16_t)(
      (f->size + UCV_UDP_FRAGMENT_PAYLOAD - 1) / UCV_UDP_FRAGMENT_PAYLOAD);

  do {
    size_t chunk = f->size - offset;
    int    frag  = 0;
    if (chunk > UCV_UDP_FRAGMENT_PAYLOAD) {
      chunk = UCV_UDP_FRAGMENT_PAYLOAD;
      frag  = 1;
    } else if (offset > 0) {
      frag = 1; /* final fragment of a fragmented frame */
    }

    uint8_t flags = 0;
    if (frag || offset > 0) {
      flags |= UCV_FLAG_FRAGMENTED;
      if (offset + chunk >= f->size)
        flags |= UCV_FLAG_LAST_FRAGMENT;
    }

    /* payload_bytes is the WHOLE frame size in every fragment, so the
     * receiver can size its reassembly buffer from the first one. */
    ucv_fill_header(h, f, self->protocol_id, (uint32_t)f->size, flags);
    h->packet_seq = im->packet_seq++;
    h->fragment_index = (uint16_t)fragments;
    h->fragment_count = fragment_count;
    memcpy(pkt + UCV_FRAME_HEADER_SIZE, f->data + offset, chunk);

    /* Stamped as late as possible — everything above is per-fragment work
     * that would otherwise be charged to the network. */
    h->t_sent_ns = ucv_now_ns();
    ucv_frame_header_finalize(h);

    ssize_t n = sendto(im->sock, pkt, UCV_FRAME_HEADER_SIZE + chunk, 0,
                       (struct sockaddr *)&im->peer, sizeof(im->peer));
    if (n < 0) {
      failed = 1;
      break;
    }
    self->bytes_wire += (uint64_t)n;
    offset += chunk;
    fragments++;
  } while (offset < f->size);

  if (failed) {
    self->send_errors++;
    return -errno;
  }
  self->frames_sent++;
  self->bytes_payload += f->size;
  return 0;
}

static void rawudp_stop(ucv_transport_t *self) {
  rawudp_impl_t *im = (rawudp_impl_t *)self->impl;
  if (im->sock >= 0) {
    close(im->sock);
    im->sock = -1;
  }
  im->started = 0;
}

static rawudp_impl_t g_rawudp_impl = {.sock = -1, .started = 0};

static ucv_transport_t g_rawudp = {
    .name        = "raw_udp",
    .protocol_id = UCV_PROTO_RAWUDP,
    .start       = rawudp_start,
    .send        = rawudp_send,
    .stop        = rawudp_stop,
    .impl        = &g_rawudp_impl,
};

ucv_transport_t *ucv_transport_rawudp(void) { return &g_rawudp; }

/* ================================================================ */
/* Registry                                                          */
/* ================================================================ */
/* Unimplemented protocols return NULL on purpose. Falling back to another
 * transport would produce a run labelled "srt" whose bytes actually went
 * over UDP — the single most damaging thing that could happen to these
 * measurements. */

ucv_transport_t *ucv_transport_get(ucv_protocol_t proto) {
  switch (proto) {
    case UCV_PROTO_RAWUDP: return ucv_transport_rawudp();
    case UCV_PROTO_MJPEG:  return ucv_transport_mjpeg();
    case UCV_PROTO_SRT:    return ucv_transport_srt();
    case UCV_PROTO_RTSP:   return ucv_transport_rtsp();
    case UCV_PROTO_RTSP_SIGNALLED: return ucv_transport_rtsp_signalled();
    case UCV_PROTO_WEBRTC: return ucv_transport_webrtc();
    case UCV_PROTO_RTMPS:  return ucv_transport_rtmps();
    case UCV_PROTO_HLS:    return ucv_transport_hls();
    default:               return NULL;
  }
}

/* Weak stubs for the library-backed transports. Each is replaced by a real
 * implementation when its library is vendored in; see
 * experiment/docs/03-implementation-plan.md. */
__attribute__((weak)) ucv_transport_t *ucv_transport_srt(void)    { return NULL; }
__attribute__((weak)) ucv_transport_t *ucv_transport_rtsp(void)   { return NULL; }
__attribute__((weak)) ucv_transport_t *ucv_transport_rtsp_signalled(void) { return NULL; }
__attribute__((weak)) ucv_transport_t *ucv_transport_webrtc(void) { return NULL; }
__attribute__((weak)) ucv_transport_t *ucv_transport_rtmps(void)  { return NULL; }
__attribute__((weak)) ucv_transport_t *ucv_transport_hls(void)    { return NULL; }
