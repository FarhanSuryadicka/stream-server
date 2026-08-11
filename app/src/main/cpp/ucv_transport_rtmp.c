/* RTMP transport (protocol #6) — the TCP high-latency contrast case.
 *
 * Deliberately included to show what a TCP-based publishing protocol costs:
 * head-of-line blocking should make p99 far worse than p50. That is a
 * prediction to verify, not to assume — if the measurement disagrees, the
 * measurement wins.
 *
 * ## Why this is labelled rtmp, not rtmps
 *
 * The "S" in RTMPS is TLS. No TLS library is vendored in this project (the SRT
 * build has encryption off for the same reason), and quietly labelling a
 * plaintext stream "RTMPS" would misreport both the security property and the
 * handshake cost. So this implements plain RTMP over TCP and says so
 * everywhere. Adding TLS later means wrapping the socket in mbedTLS and
 * renaming — the RTMP framing below does not change.
 *
 * ## What is implemented
 *
 *   - RTMP handshake (C0/C1/S0/S1/S2/C2), simple form: the version-2 digest
 *     handshake is only required by some CDNs, and a local media server
 *     (MediaMTX / nginx-rtmp / SRS) accepts the simple one.
 *   - AMF0 command chunk stream: connect -> createStream -> publish.
 *   - FLV video tags (AVC sequence header + NALUs) on chunk stream 6.
 *   - Per-frame instrumentation as an AMF0 `@setDataFrame` metadata message,
 *     since RTMP has no per-frame header of its own.
 *
 * A media server is a third box in the path, and that operational cost is
 * itself part of the finding (implementation plan section 7).
 */

#include "ucv_transport.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "ucv-rtmp"
#define MLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define MLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define RTMP_HANDSHAKE_SIZE 1536
#define RTMP_CHUNK_SIZE     4096   /* negotiated up from the 128-byte default */
#define RTMP_CSID_CONTROL   3
#define RTMP_CSID_VIDEO     6
#define RTMP_CSID_DATA      4
#define RTMP_MSG_SET_CHUNK  1
#define RTMP_MSG_AMF0_CMD   20
#define RTMP_MSG_AMF0_DATA  18
#define RTMP_MSG_VIDEO      9
#define RTMP_STREAM_ID      1

typedef struct {
  int sock;
  int started;
  int published;
  uint32_t packet_seq;
  uint64_t base_ns;          /* run start, for FLV millisecond timestamps */
  int sent_sequence_header;  /* AVC decoder config sent once, before frames */
  char app[64];
  char stream[64];
} rtmp_impl_t;

static rtmp_impl_t g_impl = {.sock = -1};

/* ---------------------------------------------------------------- */
/* Byte helpers                                                      */
/* ---------------------------------------------------------------- */

static void put_be24(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int send_all(int s, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t off = 0;
  while (off < len) {
    const ssize_t n = send(s, p + off, len - off, 0);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int recv_all(int s, void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  size_t off = 0;
  while (off < len) {
    const ssize_t n = recv(s, p + off, len - off, 0);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* AMF0 encoding                                                     */
/* ---------------------------------------------------------------- */

static size_t amf_number(uint8_t *p, double v) {
  p[0] = 0x00;
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  for (int i = 0; i < 8; i++) p[1 + i] = (uint8_t)(bits >> (56 - 8 * i));
  return 9;
}
static size_t amf_boolean(uint8_t *p, int v) {
  p[0] = 0x01; p[1] = (uint8_t)(v ? 1 : 0); return 2;
}
static size_t amf_string(uint8_t *p, const char *s) {
  const size_t n = strlen(s);
  p[0] = 0x02; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)n;
  memcpy(p + 3, s, n);
  return 3 + n;
}
static size_t amf_key(uint8_t *p, const char *s) {   /* object key: no marker */
  const size_t n = strlen(s);
  p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
  memcpy(p + 2, s, n);
  return 2 + n;
}
static size_t amf_object_start(uint8_t *p) { p[0] = 0x03; return 1; }
static size_t amf_object_end(uint8_t *p) {
  p[0] = 0; p[1] = 0; p[2] = 0x09; return 3;
}
static size_t amf_null(uint8_t *p) { p[0] = 0x05; return 1; }

/* ---------------------------------------------------------------- */
/* Chunking                                                          */
/* ---------------------------------------------------------------- */

/* Writes one RTMP message, splitting the body across chunks of RTMP_CHUNK_SIZE
 * with type-3 continuation headers. */
static int send_message(rtmp_impl_t *im, uint8_t csid, uint8_t type,
                        uint32_t timestamp, uint32_t stream_id,
                        const uint8_t *body, size_t body_len) {
  uint8_t hdr[12];
  hdr[0] = (uint8_t)(0x00 | (csid & 0x3f));      /* fmt 0, full header */
  put_be24(hdr + 1, timestamp & 0xffffff);
  put_be24(hdr + 4, (uint32_t)body_len);
  hdr[7] = type;
  /* Message stream id is LITTLE-endian here — the one field in the RTMP header
   * that is, which is a classic source of "server closes the connection with
   * no error" bugs. */
  hdr[8]  = (uint8_t)stream_id;
  hdr[9]  = (uint8_t)(stream_id >> 8);
  hdr[10] = (uint8_t)(stream_id >> 16);
  hdr[11] = (uint8_t)(stream_id >> 24);
  if (send_all(im->sock, hdr, sizeof(hdr)) < 0) return -EIO;

  size_t off = 0;
  while (off < body_len) {
    size_t chunk = body_len - off;
    if (chunk > RTMP_CHUNK_SIZE) chunk = RTMP_CHUNK_SIZE;
    if (send_all(im->sock, body + off, chunk) < 0) return -EIO;
    off += chunk;
    if (off < body_len) {
      const uint8_t cont = (uint8_t)(0xc0 | (csid & 0x3f)); /* fmt 3 */
      if (send_all(im->sock, &cont, 1) < 0) return -EIO;
    }
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* Handshake and session setup                                       */
/* ---------------------------------------------------------------- */

static int handshake(rtmp_impl_t *im) {
  uint8_t c0c1[1 + RTMP_HANDSHAKE_SIZE];
  c0c1[0] = 3;                                  /* protocol version */
  memset(c0c1 + 1, 0, 8);                       /* time + zero */
  for (int i = 9; i < 1 + RTMP_HANDSHAKE_SIZE; i++)
    c0c1[i] = (uint8_t)(i * 37);                /* deterministic filler */
  if (send_all(im->sock, c0c1, sizeof(c0c1)) < 0) return -EIO;

  uint8_t s0s1[1 + RTMP_HANDSHAKE_SIZE];
  if (recv_all(im->sock, s0s1, sizeof(s0s1)) < 0) return -EIO;
  if (s0s1[0] != 3) {
    MLOGE("rtmp: server offered version %u, expected 3", s0s1[0]);
    return -EPROTO;
  }
  /* C2 echoes S1. */
  if (send_all(im->sock, s0s1 + 1, RTMP_HANDSHAKE_SIZE) < 0) return -EIO;
  uint8_t s2[RTMP_HANDSHAKE_SIZE];
  if (recv_all(im->sock, s2, sizeof(s2)) < 0) return -EIO;
  return 0;
}

static int send_set_chunk_size(rtmp_impl_t *im) {
  uint8_t body[4];
  put_be32(body, RTMP_CHUNK_SIZE);
  return send_message(im, 2, RTMP_MSG_SET_CHUNK, 0, 0, body, sizeof(body));
}

static int send_connect(rtmp_impl_t *im, const char *host, int port) {
  uint8_t b[512];
  size_t n = 0;
  n += amf_string(b + n, "connect");
  n += amf_number(b + n, 1);                    /* transaction id */
  n += amf_object_start(b + n);
  n += amf_key(b + n, "app");     n += amf_string(b + n, im->app);
  n += amf_key(b + n, "type");    n += amf_string(b + n, "nonprivate");
  n += amf_key(b + n, "flashVer"); n += amf_string(b + n, "FMLE/3.0 (ucv)");
  char url[160];
  snprintf(url, sizeof(url), "rtmp://%s:%d/%s", host, port, im->app);
  n += amf_key(b + n, "tcUrl");   n += amf_string(b + n, url);
  n += amf_key(b + n, "fpad");    n += amf_boolean(b + n, 0);
  n += amf_object_end(b + n);
  return send_message(im, RTMP_CSID_CONTROL, RTMP_MSG_AMF0_CMD, 0, 0, b, n);
}

static int send_create_stream(rtmp_impl_t *im) {
  uint8_t b[64];
  size_t n = 0;
  n += amf_string(b + n, "createStream");
  n += amf_number(b + n, 2);
  n += amf_null(b + n);
  return send_message(im, RTMP_CSID_CONTROL, RTMP_MSG_AMF0_CMD, 0, 0, b, n);
}

static int send_publish(rtmp_impl_t *im) {
  uint8_t b[256];
  size_t n = 0;
  n += amf_string(b + n, "publish");
  n += amf_number(b + n, 3);
  n += amf_null(b + n);
  n += amf_string(b + n, im->stream);
  n += amf_string(b + n, "live");
  return send_message(im, RTMP_CSID_CONTROL, RTMP_MSG_AMF0_CMD, 0,
                      RTMP_STREAM_ID, b, n);
}

/* Drains whatever the server sent (window ack size, _result, onStatus...).
 * The replies are not parsed: this is a one-way publisher and the only thing
 * that matters is that the server has not closed the connection. Parsing them
 * would add an AMF decoder for no measurement benefit. */
static void drain_server(rtmp_impl_t *im, int timeout_ms) {
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(im->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t scratch[2048];
  for (;;) {
    const ssize_t n = recv(im->sock, scratch, sizeof(scratch), 0);
    if (n <= 0) break;
    if ((size_t)n < sizeof(scratch)) break;
  }
}

/* ---------------------------------------------------------------- */
/* FLV video tags                                                    */
/* ---------------------------------------------------------------- */

/* Finds SPS and PPS in an Annex-B access unit so the AVC decoder configuration
 * record can be built. Without it no player can decode the stream. */
static void find_parameter_sets(const uint8_t *d, size_t n,
                                const uint8_t **sps, size_t *sps_n,
                                const uint8_t **pps, size_t *pps_n) {
  *sps = *pps = NULL; *sps_n = *pps_n = 0;
  size_t i = 0;
  while (i + 4 <= n) {
    size_t sc = 0;
    if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) sc = 3;
    else if (i + 4 <= n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) sc = 4;
    if (!sc) { i++; continue; }
    const size_t start = i + sc;
    size_t j = start;
    while (j + 3 <= n) {
      if (d[j] == 0 && d[j+1] == 0 && (d[j+2] == 1 ||
          (j + 4 <= n && d[j+2] == 0 && d[j+3] == 1))) break;
      j++;
    }
    const size_t len = (j >= n ? n : j) - start;
    if (len) {
      const uint8_t type = d[start] & 0x1f;
      if (type == 7) { *sps = d + start; *sps_n = len; }
      else if (type == 8) { *pps = d + start; *pps_n = len; }
    }
    i = (j >= n) ? n : j;
  }
}

/* Converts Annex-B start codes to the 4-byte length prefixes FLV requires. */
static size_t annexb_to_avcc(const uint8_t *d, size_t n, uint8_t *out,
                             size_t out_cap, int skip_parameter_sets) {
  size_t w = 0, i = 0;
  while (i + 3 <= n) {
    size_t sc = 0;
    if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) sc = 3;
    else if (i + 4 <= n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) sc = 4;
    if (!sc) { i++; continue; }
    const size_t start = i + sc;
    size_t j = start;
    while (j + 3 <= n) {
      if (d[j] == 0 && d[j+1] == 0 && (d[j+2] == 1 ||
          (j + 4 <= n && d[j+2] == 0 && d[j+3] == 1))) break;
      j++;
    }
    const size_t end = (j + 3 > n) ? n : j;
    const size_t len = end - start;
    if (len) {
      const uint8_t type = d[start] & 0x1f;
      /* SPS/PPS live in the sequence header, not in every frame. */
      const int drop = skip_parameter_sets && (type == 7 || type == 8);
      if (!drop) {
        if (w + 4 + len > out_cap) return w;
        put_be32(out + w, (uint32_t)len);
        memcpy(out + w + 4, d + start, len);
        w += 4 + len;
      }
    }
    i = end;
  }
  return w;
}

static int send_sequence_header(rtmp_impl_t *im, const uint8_t *sps,
                                size_t sps_n, const uint8_t *pps, size_t pps_n) {
  uint8_t b[512];
  size_t n = 0;
  b[n++] = 0x17;              /* keyframe | AVC */
  b[n++] = 0x00;              /* AVC sequence header */
  b[n++] = 0; b[n++] = 0; b[n++] = 0;   /* composition time */
  b[n++] = 0x01;              /* configurationVersion */
  b[n++] = sps_n > 1 ? sps[1] : 0x42;   /* profile */
  b[n++] = sps_n > 2 ? sps[2] : 0x00;   /* compatibility */
  b[n++] = sps_n > 3 ? sps[3] : 0x1f;   /* level */
  b[n++] = 0xff;              /* 6 bits reserved | 4-byte NAL length - 1 */
  b[n++] = 0xe1;              /* 3 bits reserved | 1 SPS */
  b[n++] = (uint8_t)(sps_n >> 8); b[n++] = (uint8_t)sps_n;
  memcpy(b + n, sps, sps_n); n += sps_n;
  b[n++] = 0x01;              /* 1 PPS */
  b[n++] = (uint8_t)(pps_n >> 8); b[n++] = (uint8_t)pps_n;
  memcpy(b + n, pps, pps_n); n += pps_n;
  return send_message(im, RTMP_CSID_VIDEO, RTMP_MSG_VIDEO, 0,
                      RTMP_STREAM_ID, b, n);
}

/* ---------------------------------------------------------------- */
/* Transport interface                                               */
/* ---------------------------------------------------------------- */

/* cfg is "<host>:<port>" and optionally "<host>:<port>/<app>/<stream>". */
static int rtmp_start(ucv_transport_t *self, const char *cfg) {
  rtmp_impl_t *im = (rtmp_impl_t *)self->impl;
  if (im->started) return 0;

  char host[64] = {0};
  int port = UCV_PORT_RTMPS;
  snprintf(im->app, sizeof(im->app), "live");
  snprintf(im->stream, sizeof(im->stream), "ucv");

  if (cfg && *cfg) {
    char work[192];
    snprintf(work, sizeof(work), "%s", cfg);
    char *slash = strchr(work, '/');
    if (slash) {
      *slash = '\0';
      char *second = strchr(slash + 1, '/');
      if (second) {
        *second = '\0';
        snprintf(im->app, sizeof(im->app), "%s", slash + 1);
        snprintf(im->stream, sizeof(im->stream), "%s", second + 1);
      } else {
        snprintf(im->app, sizeof(im->app), "%s", slash + 1);
      }
    }
    char *colon = strrchr(work, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); }
    snprintf(host, sizeof(host), "%s", work);
  }
  if (!host[0] || port <= 0 || port > 65535) return -EINVAL;

  im->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (im->sock < 0) return -errno;
  /* Nagle would batch small video tags and add tens of milliseconds that
   * belong to a socket option rather than to RTMP, unfairly penalising it. */
  int one = 1;
  setsockopt(im->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(im->sock); im->sock = -1; return -EINVAL;
  }
  if (connect(im->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    const int e = errno;
    close(im->sock); im->sock = -1;
    MLOGE("rtmp: connect %s:%d failed errno=%d", host, port, e);
    return -e;
  }

  int rc = handshake(im);
  if (rc < 0) { close(im->sock); im->sock = -1; return rc; }
  if (send_set_chunk_size(im) < 0 || send_connect(im, host, port) < 0) {
    close(im->sock); im->sock = -1; return -EIO;
  }
  drain_server(im, 1000);
  if (send_create_stream(im) < 0) { close(im->sock); im->sock = -1; return -EIO; }
  drain_server(im, 1000);
  if (send_publish(im) < 0) { close(im->sock); im->sock = -1; return -EIO; }
  drain_server(im, 1000);

  im->started = 1;
  im->published = 1;
  im->sent_sequence_header = 0;
  im->packet_seq = 0;
  im->base_ns = ucv_now_ns();
  MLOGI("rtmp: publishing to %s:%d/%s/%s (PLAINTEXT - not rtmps)",
        host, port, im->app, im->stream);
  return 0;
}

static int rtmp_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  rtmp_impl_t *im = (rtmp_impl_t *)self->impl;
  if (!im->started || !im->published) return -ENOTCONN;

  const uint32_t ts_ms =
      (uint32_t)((f->t_capture_ns - im->base_ns) / 1000000ull);

  if (!im->sent_sequence_header) {
    const uint8_t *sps, *pps; size_t sps_n, pps_n;
    find_parameter_sets(f->data, f->size, &sps, &sps_n, &pps, &pps_n);
    if (!sps || !pps) {
      /* Without SPS/PPS the stream is undecodable. The encoder prepends codec
       * config to keyframes, so this only happens before the first keyframe —
       * skip rather than publish garbage a server would reject. */
      return 0;
    }
    if (send_sequence_header(im, sps, sps_n, pps, pps_n) < 0) {
      self->send_errors++;
      return -EIO;
    }
    im->sent_sequence_header = 1;
  }

  /* Per-frame instrumentation: RTMP has no frame header, so it rides in an
   * AMF0 data message immediately before its video tag. */
  ucv_frame_header_t h;
  ucv_fill_header(&h, f, UCV_PROTO_RTMPS, (uint32_t)f->size,
                  f->is_keyframe ? UCV_FLAG_KEYFRAME : 0);
  h.packet_seq = im->packet_seq++;
  h.fragment_index = 0;
  h.fragment_count = 1;
  h.t_sent_ns = ucv_now_ns();
  ucv_frame_header_finalize(&h);

  uint8_t meta[256];
  size_t mn = 0;
  mn += amf_string(meta + mn, "@setDataFrame");
  mn += amf_string(meta + mn, "onUCV");
  mn += amf_object_start(meta + mn);
  mn += amf_key(meta + mn, "seq");  mn += amf_number(meta + mn, (double)h.frame_seq);
  mn += amf_key(meta + mn, "cap");  mn += amf_number(meta + mn, (double)h.t_capture_ns);
  mn += amf_key(meta + mn, "enc");  mn += amf_number(meta + mn, (double)h.t_encoded_ns);
  mn += amf_key(meta + mn, "snd");  mn += amf_number(meta + mn, (double)h.t_sent_ns);
  mn += amf_object_end(meta + mn);
  if (send_message(im, RTMP_CSID_DATA, RTMP_MSG_AMF0_DATA, ts_ms,
                   RTMP_STREAM_ID, meta, mn) < 0) {
    self->send_errors++;
    return -EIO;
  }

  /* FLV video tag: 5-byte header then length-prefixed NAL units. */
  static uint8_t tag[1024 * 1024];
  if (f->size + 5 > sizeof(tag)) { self->send_errors++; return -EMSGSIZE; }
  tag[0] = (uint8_t)(f->is_keyframe ? 0x17 : 0x27);
  tag[1] = 0x01;                            /* AVC NALU */
  put_be24(tag + 2, 0);                     /* composition time offset */
  const size_t body = annexb_to_avcc(f->data, f->size, tag + 5,
                                     sizeof(tag) - 5, 1);
  if (!body) { self->send_errors++; return -EINVAL; }

  /* t_sent is stamped as late as possible, immediately before the write. */
  if (send_message(im, RTMP_CSID_VIDEO, RTMP_MSG_VIDEO, ts_ms,
                   RTMP_STREAM_ID, tag, body + 5) < 0) {
    self->send_errors++;
    return -EIO;
  }
  self->frames_sent++;
  self->bytes_payload += f->size;
  self->bytes_wire += body + 5 + mn + 24;   /* + chunk headers, approximate */
  return 0;
}

static void rtmp_stop(ucv_transport_t *self) {
  rtmp_impl_t *im = (rtmp_impl_t *)self->impl;
  if (!im->started) return;
  if (im->sock >= 0) {
    shutdown(im->sock, SHUT_RDWR);
    close(im->sock);
    im->sock = -1;
  }
  im->started = 0;
  im->published = 0;
  im->sent_sequence_header = 0;
}

static ucv_transport_t g_rtmp = {
    /* Named "rtmp", not "rtmps": there is no TLS here (see the file header).
     * A run must never claim a security property it does not have. */
    .name = "rtmp", .protocol_id = UCV_PROTO_RTMPS,
    .start = rtmp_start, .send = rtmp_send, .stop = rtmp_stop,
    .impl = &g_impl,
};

ucv_transport_t *ucv_transport_rtmps(void) { return &g_rtmp; }

/* Exposed for the smoke test only. */
size_t ucv_rtmp_annexb_to_avcc(const uint8_t *d, size_t n, uint8_t *out,
                               size_t cap, int skip_ps) {
  return annexb_to_avcc(d, n, out, cap, skip_ps);
}
