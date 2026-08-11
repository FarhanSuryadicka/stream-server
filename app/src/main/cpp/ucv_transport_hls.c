/* HLS transport (protocol #7) — the high-latency contrast case.
 *
 * Included deliberately to show what segment-based delivery costs: expect
 * seconds, not milliseconds. Segment duration dominates the latency and is the
 * only real tuning knob, so it is reported in every log line and kept in one
 * constant below.
 *
 * Pipeline: H.264 access units -> MPEG-TS segments -> in-memory ring ->
 * HTTP GET /live.m3u8 and /segN.ts.
 *
 * MPEG-TS rather than fMP4: TS needs no initialisation segment and no seek
 * table, so a segment is self-contained and the muxer stays small enough to
 * audit. Both are equally "HLS" for latency purposes, which is what is being
 * measured.
 *
 * ## Where the measurement header goes
 *
 * HLS has no per-frame metadata channel — this is a real limitation of the
 * protocol, not of this implementation. The per-frame UCV header is therefore
 * written to a per-segment sidecar (`/segN.json`) exactly as
 * experiment/docs/03-implementation-plan.md section 8 specifies, and the
 * receiver reads it alongside the segment. Timestamps stay per-frame; only
 * their delivery is batched.
 */

#include "ucv_transport.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "ucv-hls"
#define HLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define HLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Segment duration drives HLS latency more than anything else. 1 s is
 * aggressive for HLS (players typically buffer 3 segments) and is chosen to
 * show the protocol at its best rather than to flatter the alternatives. */
#define HLS_SEGMENT_MS      1000
#define HLS_WINDOW          6      /* segments kept live in the playlist */
#define HLS_SEGMENT_CAP     (3u * 1024u * 1024u)
#define HLS_TS_PACKET       188
#define HLS_PID_PMT         0x1000
#define HLS_PID_VIDEO       0x0100
#define HLS_MAX_FRAME_META  512    /* per-frame headers in one segment sidecar */

typedef struct {
  uint8_t  *data;
  size_t    size;
  uint32_t  index;
  uint32_t  frames;
  uint64_t  duration_ms;
  char      meta[HLS_MAX_FRAME_META * 96];
  size_t    meta_len;
} hls_segment_t;

typedef struct {
  int listen_sock;
  pthread_t http_thread;
  volatile int quit;
  int started;
  int port;

  hls_segment_t window[HLS_WINDOW];
  uint32_t next_index;          /* index of the segment being filled */
  uint32_t media_sequence;      /* EXT-X-MEDIA-SEQUENCE of window[0] */
  pthread_mutex_t lock;

  /* Segment under construction. */
  uint8_t  *pending;
  size_t    pending_size, pending_cap;
  uint32_t  pending_frames;
  uint64_t  segment_start_ns;
  char      pending_meta[HLS_MAX_FRAME_META * 96];
  size_t    pending_meta_len;

  uint8_t   cc_video;           /* TS continuity counter */
  uint8_t   cc_pat, cc_pmt;
  uint32_t  packet_seq;
} hls_impl_t;

static hls_impl_t g_impl = {.listen_sock = -1, .lock = PTHREAD_MUTEX_INITIALIZER};

/* ---------------------------------------------------------------- */
/* MPEG-TS muxing                                                    */
/* ---------------------------------------------------------------- */

static uint32_t crc32_mpeg(const uint8_t *data, size_t len) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint32_t)data[i] << 24;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : (crc << 1);
  }
  return crc;
}

static int seg_reserve(hls_impl_t *im, size_t extra) {
  if (im->pending_size + extra <= im->pending_cap) return 0;
  size_t want = im->pending_cap ? im->pending_cap * 2 : 256u * 1024u;
  while (want < im->pending_size + extra) want *= 2;
  if (want > HLS_SEGMENT_CAP) return -ENOSPC;
  uint8_t *p = (uint8_t *)realloc(im->pending, want);
  if (!p) return -ENOMEM;
  im->pending = p;
  im->pending_cap = want;
  return 0;
}

static int seg_append(hls_impl_t *im, const uint8_t *data, size_t len) {
  const int rc = seg_reserve(im, len);
  if (rc < 0) return rc;
  memcpy(im->pending + im->pending_size, data, len);
  im->pending_size += len;
  return 0;
}

/* PAT and PMT are repeated at the head of every segment: a player may join at
 * any segment boundary, and a segment that cannot be decoded standalone is not
 * a valid HLS segment. */
static int write_pat(hls_impl_t *im) {
  uint8_t pkt[HLS_TS_PACKET];
  memset(pkt, 0xff, sizeof(pkt));
  pkt[0] = 0x47; pkt[1] = 0x40; pkt[2] = 0x00;
  pkt[3] = (uint8_t)(0x10 | (im->cc_pat++ & 0x0f));
  pkt[4] = 0x00; /* pointer field */
  uint8_t *s = pkt + 5;
  s[0] = 0x00;                 /* table id: PAT */
  s[1] = 0xb0; s[2] = 0x0d;    /* section length 13 */
  s[3] = 0x00; s[4] = 0x01;    /* transport stream id */
  s[5] = 0xc1; s[6] = 0x00; s[7] = 0x00;
  s[8] = 0x00; s[9] = 0x01;    /* program number 1 */
  s[10] = (uint8_t)(0xe0 | (HLS_PID_PMT >> 8));
  s[11] = (uint8_t)(HLS_PID_PMT & 0xff);
  const uint32_t crc = crc32_mpeg(s, 12);
  s[12] = (uint8_t)(crc >> 24); s[13] = (uint8_t)(crc >> 16);
  s[14] = (uint8_t)(crc >> 8);  s[15] = (uint8_t)crc;
  return seg_append(im, pkt, sizeof(pkt));
}

static int write_pmt(hls_impl_t *im) {
  uint8_t pkt[HLS_TS_PACKET];
  memset(pkt, 0xff, sizeof(pkt));
  pkt[0] = 0x47;
  pkt[1] = (uint8_t)(0x40 | (HLS_PID_PMT >> 8));
  pkt[2] = (uint8_t)(HLS_PID_PMT & 0xff);
  pkt[3] = (uint8_t)(0x10 | (im->cc_pmt++ & 0x0f));
  pkt[4] = 0x00;
  uint8_t *s = pkt + 5;
  s[0] = 0x02;                 /* table id: PMT */
  s[1] = 0xb0; s[2] = 0x12;    /* section length 18 */
  s[3] = 0x00; s[4] = 0x01;
  s[5] = 0xc1; s[6] = 0x00; s[7] = 0x00;
  s[8] = (uint8_t)(0xe0 | (HLS_PID_VIDEO >> 8));
  s[9] = (uint8_t)(HLS_PID_VIDEO & 0xff);   /* PCR pid */
  s[10] = 0xf0; s[11] = 0x00;               /* program info length */
  s[12] = 0x1b;                             /* stream type: H.264 */
  s[13] = (uint8_t)(0xe0 | (HLS_PID_VIDEO >> 8));
  s[14] = (uint8_t)(HLS_PID_VIDEO & 0xff);
  s[15] = 0xf0; s[16] = 0x00;
  const uint32_t crc = crc32_mpeg(s, 17);
  s[17] = (uint8_t)(crc >> 24); s[18] = (uint8_t)(crc >> 16);
  s[19] = (uint8_t)(crc >> 8);  s[20] = (uint8_t)crc;
  return seg_append(im, pkt, sizeof(pkt));
}

static void write_pts(uint8_t *p, uint8_t prefix, uint64_t pts) {
  p[0] = (uint8_t)(prefix | (((pts >> 30) & 0x07) << 1) | 1);
  p[1] = (uint8_t)((pts >> 22) & 0xff);
  p[2] = (uint8_t)((((pts >> 15) & 0x7f) << 1) | 1);
  p[3] = (uint8_t)((pts >> 7) & 0xff);
  p[4] = (uint8_t)(((pts & 0x7f) << 1) | 1);
}

/* Wraps one access unit in a PES packet and splits it across TS packets. */
static int write_video_pes(hls_impl_t *im, const uint8_t *au, size_t au_size,
                           uint64_t pts90k, int keyframe) {
  uint8_t pes[19];
  size_t pes_len = 0;
  pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xe0; /* video stream */
  /* Length 0 is legal for video and avoids failing on units above 65535
   * bytes, which keyframes routinely exceed. */
  pes[4] = 0x00; pes[5] = 0x00;
  pes[6] = 0x80; pes[7] = 0x80; pes[8] = 0x05;
  write_pts(pes + 9, 0x20, pts90k);
  pes_len = 14;

  const uint8_t *chunks[2] = {pes, au};
  const size_t sizes[2] = {pes_len, au_size};
  size_t chunk = 0, offset = 0;
  int first = 1;

  while (chunk < 2) {
    uint8_t pkt[HLS_TS_PACKET];
    size_t pos = 4;
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)((first ? 0x40 : 0x00) | (HLS_PID_VIDEO >> 8));
    pkt[2] = (uint8_t)(HLS_PID_VIDEO & 0xff);

    size_t remaining = 0;
    for (size_t c = chunk, o = offset; c < 2; c++, o = 0)
      remaining += sizes[c] - o;

    /* Work out the adaptation field first, because its length decides how much
     * payload fits. Two independent reasons to need one:
     *   - the first packet of a keyframe carries PCR + random-access indicator,
     *     without which a player cannot start decoding at this segment;
     *   - the last packet of an access unit is usually short and TS packets are
     *     fixed at 188 bytes, so the slack must be stuffed.
     * af_len counts the bytes AFTER the length byte itself. */
    const int want_pcr = (first && keyframe);
    size_t af_len = want_pcr ? 7 : 0;          /* flags byte + 6 PCR bytes */
    const size_t payload_room = HLS_TS_PACKET - 4 - (af_len ? af_len + 1 : 0);
    if (remaining < payload_room) {
      /* Grow the adaptation field to absorb exactly the shortfall. */
      af_len += payload_room - remaining;
      if (!want_pcr) {
        /* Going from "no adaptation field" to "one byte of it" costs the length
         * byte too, so one byte of shortfall needs af_len == 0. */
        af_len = (payload_room - remaining) - 1;
      }
    }
    const int has_af = want_pcr || (remaining < payload_room);

    if (has_af) {
      pkt[3] = (uint8_t)(0x30 | (im->cc_video++ & 0x0f));
      pkt[4] = (uint8_t)af_len;
      if (af_len) {
        pkt[5] = (uint8_t)(want_pcr ? 0x50 : 0x00);  /* RAI | PCR present */
        if (want_pcr) {
          const uint64_t pcr = pts90k;
          pkt[6]  = (uint8_t)(pcr >> 25);
          pkt[7]  = (uint8_t)(pcr >> 17);
          pkt[8]  = (uint8_t)(pcr >> 9);
          pkt[9]  = (uint8_t)(pcr >> 1);
          pkt[10] = (uint8_t)(((pcr & 1) << 7) | 0x7e);
          pkt[11] = 0x00;
          if (af_len > 7) memset(pkt + 12, 0xff, af_len - 7);
        } else {
          memset(pkt + 6, 0xff, af_len - 1);
        }
      }
      pos = 5 + af_len;
    } else {
      pkt[3] = (uint8_t)(0x10 | (im->cc_video++ & 0x0f));
    }

    while (pos < HLS_TS_PACKET && chunk < 2) {
      const size_t avail = sizes[chunk] - offset;
      if (!avail) { chunk++; offset = 0; continue; }
      size_t take = HLS_TS_PACKET - pos;
      if (take > avail) take = avail;
      memcpy(pkt + pos, chunks[chunk] + offset, take);
      pos += take; offset += take;
      if (offset == sizes[chunk]) { chunk++; offset = 0; }
    }
    if (pos < HLS_TS_PACKET) memset(pkt + pos, 0xff, HLS_TS_PACKET - pos);

    const int rc = seg_append(im, pkt, HLS_TS_PACKET);
    if (rc < 0) return rc;
    first = 0;
  }
  return 0;
}

/* Publishes the pending segment into the live window. */
static void seal_segment(hls_impl_t *im, uint64_t now_ns) {
  if (!im->pending_size) return;
  pthread_mutex_lock(&im->lock);
  hls_segment_t *slot = &im->window[im->next_index % HLS_WINDOW];
  free(slot->data);
  slot->data = (uint8_t *)malloc(im->pending_size);
  if (slot->data) {
    memcpy(slot->data, im->pending, im->pending_size);
    slot->size = im->pending_size;
    slot->index = im->next_index;
    slot->frames = im->pending_frames;
    slot->duration_ms = (now_ns - im->segment_start_ns) / 1000000ull;
    if (!slot->duration_ms) slot->duration_ms = HLS_SEGMENT_MS;
    if (im->pending_meta_len) {
      memcpy(slot->meta, im->pending_meta, im->pending_meta_len);
      slot->meta_len = im->pending_meta_len;
      /* pending_meta starts with "[\n" and contains comma-separated entries.
       * Close it only when the segment is sealed so more frames can be
       * appended without repeatedly removing a bracket. */
      if (slot->meta_len + 3 <= sizeof(slot->meta)) {
        memcpy(slot->meta + slot->meta_len, "\n]\n", 3);
        slot->meta_len += 3;
      }
    } else {
      memcpy(slot->meta, "[]\n", 3);
      slot->meta_len = 3;
    }
  }
  im->next_index++;
  if (im->next_index > HLS_WINDOW)
    im->media_sequence = im->next_index - HLS_WINDOW;
  pthread_mutex_unlock(&im->lock);

  im->pending_size = 0;
  im->pending_frames = 0;
  im->pending_meta_len = 0;
  im->segment_start_ns = now_ns;
}

/* ---------------------------------------------------------------- */
/* HTTP serving                                                      */
/* ---------------------------------------------------------------- */

static int http_send(int s, const void *buf, size_t len) {
  size_t off = 0;
  const char *p = (const char *)buf;
  while (off < len) {
    const ssize_t n = send(s, p + off, len - off, 0);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static void serve_playlist(hls_impl_t *im, int s) {
  char body[1024];
  size_t n = 0;
  pthread_mutex_lock(&im->lock);
  const uint32_t first = im->media_sequence;
  uint64_t max_duration_ms = HLS_SEGMENT_MS;
  for (uint32_t i = first; i < im->next_index; i++) {
    const hls_segment_t *seg = &im->window[i % HLS_WINDOW];
    if (seg->data && seg->index == i && seg->duration_ms > max_duration_ms)
      max_duration_ms = seg->duration_ms;
  }
  const unsigned target_duration =
      (unsigned)((max_duration_ms + 999) / 1000);
  n += (size_t)snprintf(body + n, sizeof(body) - n,
      "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%u\n"
      "#EXT-X-MEDIA-SEQUENCE:%u\n",
      target_duration, first);
  for (uint32_t i = first; i < im->next_index && n < sizeof(body) - 96; i++) {
    const hls_segment_t *seg = &im->window[i % HLS_WINDOW];
    if (!seg->data || seg->index != i) continue;
    n += (size_t)snprintf(body + n, sizeof(body) - n,
        "#EXTINF:%.3f,\nseg%u.ts\n", seg->duration_ms / 1000.0, i);
  }
  pthread_mutex_unlock(&im->lock);

  char head[192];
  const int hn = snprintf(head, sizeof(head),
      "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n"
      "Content-Length: %zu\r\nCache-Control: no-cache\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n", n);
  if (http_send(s, head, (size_t)hn) == 0) http_send(s, body, n);
}

static void serve_segment(hls_impl_t *im, int s, uint32_t index, int meta) {
  pthread_mutex_lock(&im->lock);
  const hls_segment_t *seg = &im->window[index % HLS_WINDOW];
  const int have = seg->data && seg->index == index;
  uint8_t *copy = NULL; size_t copy_n = 0;
  char metacopy[sizeof(seg->meta)]; size_t meta_n = 0;
  if (have) {
    if (meta) {
      meta_n = seg->meta_len;
      memcpy(metacopy, seg->meta, meta_n);
    } else {
      copy_n = seg->size;
      copy = (uint8_t *)malloc(copy_n);
      if (copy) memcpy(copy, seg->data, copy_n);
    }
  }
  pthread_mutex_unlock(&im->lock);

  if (!have || (!meta && !copy)) {
    static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    http_send(s, nf, sizeof(nf) - 1);
    free(copy);
    return;
  }
  char head[192];
  const int hn = snprintf(head, sizeof(head),
      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n",
      meta ? "application/json" : "video/mp2t", meta ? meta_n : copy_n);
  if (http_send(s, head, (size_t)hn) == 0)
    http_send(s, meta ? (const void *)metacopy : (const void *)copy,
              meta ? meta_n : copy_n);
  free(copy);
}

static void *http_thread(void *arg) {
  hls_impl_t *im = (hls_impl_t *)arg;
  HLOGI("hls: serving on :%d (segment %d ms)", im->port, HLS_SEGMENT_MS);
  while (!im->quit) {
    const int cs = accept(im->listen_sock, NULL, NULL);
    if (cs < 0) { if (im->quit) break; continue; }
    char req[1024];
    const ssize_t n = recv(cs, req, sizeof(req) - 1, 0);
    if (n > 0) {
      req[n] = '\0';
      char path[256] = {0};
      if (sscanf(req, "GET %255s", path) == 1) {
        unsigned idx = 0;
        /* Dispatch on the SUFFIX, not on sscanf's return value: "%u.ts" also
         * matches "/seg0.json", because a trailing literal that fails to match
         * is not reported as a conversion failure. That silently served TS
         * bytes as the JSON sidecar. */
        const char *dot = strrchr(path, '.');
        const int is_ts   = dot && strcmp(dot, ".ts") == 0;
        const int is_json = dot && strcmp(dot, ".json") == 0;
        if (strstr(path, ".m3u8")) {
          serve_playlist(im, cs);
        } else if (is_ts && sscanf(path, "/seg%u.", &idx) == 1) {
          serve_segment(im, cs, idx, 0);
        } else if (is_json && sscanf(path, "/seg%u.", &idx) == 1) {
          serve_segment(im, cs, idx, 1);
        } else {
          static const char nf[] =
              "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
          http_send(cs, nf, sizeof(nf) - 1);
        }
      }
    }
    close(cs);
  }
  HLOGI("hls: http thread exiting");
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Transport interface                                               */
/* ---------------------------------------------------------------- */

static int hls_start(ucv_transport_t *self, const char *cfg) {
  hls_impl_t *im = (hls_impl_t *)self->impl;
  if (im->started) return 0;
  im->port = UCV_PORT_HLS;
  const char *colon = cfg ? strrchr(cfg, ':') : NULL;
  if (colon) {
    const int p = atoi(colon + 1);
    if (p > 0 && p <= 65535) im->port = p;
  }
  im->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (im->listen_sock < 0) return -errno;
  int one = 1;
  setsockopt(im->listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)im->port);
  if (bind(im->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    const int e = errno;
    close(im->listen_sock); im->listen_sock = -1;
    HLOGE("hls: bind :%d failed errno=%d", im->port, e);
    return -e;
  }
  listen(im->listen_sock, 4);
  im->quit = 0;
  im->next_index = 0;
  im->media_sequence = 0;
  im->pending_size = 0;
  im->pending_frames = 0;
  im->pending_meta_len = 0;
  im->packet_seq = 0;
  im->segment_start_ns = ucv_now_ns();
  if (pthread_create(&im->http_thread, NULL, http_thread, im) != 0) {
    close(im->listen_sock); im->listen_sock = -1;
    return -EAGAIN;
  }
  im->started = 1;
  return 0;
}

static int hls_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  hls_impl_t *im = (hls_impl_t *)self->impl;
  if (!im->started) return -ENOTCONN;

  /* A segment may only start on a keyframe, or players cannot decode it from
   * the beginning. The timer therefore arms the cut and the next keyframe
   * performs it. */
  const uint64_t now = ucv_now_ns();
  const uint64_t elapsed_ms = (now - im->segment_start_ns) / 1000000ull;
  if (elapsed_ms >= HLS_SEGMENT_MS && f->is_keyframe && im->pending_size)
    seal_segment(im, now);

  /* The first frame of every segment must be a random-access point. This also
   * recovers cleanly after an oversized segment is discarded below. */
  if (!im->pending_size && !f->is_keyframe)
    return -ENOTCONN;

  const size_t pending_before = im->pending_size;
  if (!im->pending_size) {
    if (write_pat(im) < 0 || write_pmt(im) < 0) {
      self->send_errors++;
      return -ENOSPC;
    }
  }

  const uint64_t pts90k = (f->t_capture_ns / 100000ull) * 9ull;
  const int rc = write_video_pes(im, f->data, f->size, pts90k, f->is_keyframe);
  if (rc < 0) {
    /* A segment that outgrew its cap is dropped whole rather than truncated:
     * half a segment decodes into garbage that would be indistinguishable from
     * network loss in the results. */
    self->send_errors++;
    im->pending_size = 0;
    im->pending_frames = 0;
    im->pending_meta_len = 0;
    return rc;
  }

  /* Per-frame instrumentation, batched into the segment sidecar (plan §8). */
  ucv_frame_header_t h;
  ucv_fill_header(&h, f, UCV_PROTO_HLS, (uint32_t)f->size,
                  f->is_keyframe ? UCV_FLAG_KEYFRAME : 0);
  h.packet_seq = im->packet_seq++;
  h.fragment_index = 0;
  h.fragment_count = 1;
  h.t_sent_ns = ucv_now_ns();
  ucv_frame_header_finalize(&h);
  if (im->pending_meta_len + 3 < sizeof(im->pending_meta)) {
    /* Keep three bytes reserved for the closing "\n]\n" written by
     * seal_segment(). snprintf() returns the size it wanted, so only advance
     * the cursor when the complete record fit; otherwise a truncated record
     * would make the sidecar invalid and could move the cursor past the array. */
    const size_t available =
        sizeof(im->pending_meta) - im->pending_meta_len - 3;
    const int written = snprintf(
        im->pending_meta + im->pending_meta_len, available,
        "%s{\"seq\":%u,\"cap\":%llu,\"enc\":%llu,\"snd\":%llu,"
        "\"bytes\":%u,\"key\":%s}",
        im->pending_meta_len ? ",\n" : "[\n",
        h.frame_seq, (unsigned long long)h.t_capture_ns,
        (unsigned long long)h.t_encoded_ns, (unsigned long long)h.t_sent_ns,
        (unsigned)f->size, f->is_keyframe ? "true" : "false");
    if (written > 0 && (size_t)written < available)
      im->pending_meta_len += (size_t)written;
  }

  im->pending_frames++;
  self->frames_sent++;
  self->bytes_payload += f->size;
  /* Count only TS bytes produced for this frame. Adding pending_size on every
   * frame counts the beginning of the segment repeatedly and grows roughly
   * quadratically with its frame count. */
  self->bytes_wire += im->pending_size - pending_before;
  return 0;
}

static void hls_stop(ucv_transport_t *self) {
  hls_impl_t *im = (hls_impl_t *)self->impl;
  if (!im->started) return;
  im->quit = 1;
  if (im->listen_sock >= 0) {
    shutdown(im->listen_sock, SHUT_RDWR);
    close(im->listen_sock);
    im->listen_sock = -1;
  }
  pthread_join(im->http_thread, NULL);
  pthread_mutex_lock(&im->lock);
  for (int i = 0; i < HLS_WINDOW; i++) {
    free(im->window[i].data);
    im->window[i].data = NULL;
    im->window[i].size = 0;
  }
  pthread_mutex_unlock(&im->lock);
  free(im->pending);
  im->pending = NULL;
  im->pending_cap = im->pending_size = 0;
  im->started = 0;
}

static ucv_transport_t g_hls = {
    .name = "hls", .protocol_id = UCV_PROTO_HLS,
    .start = hls_start, .send = hls_send, .stop = hls_stop,
    .impl = &g_impl,
};

ucv_transport_t *ucv_transport_hls(void) { return &g_hls; }

/* Exposed so the smoke test can assert segment/playlist behaviour without a
 * camera; not part of the transport interface. */
int ucv_hls_segment_ms(void) { return HLS_SEGMENT_MS; }
