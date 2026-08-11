/* Experiment pipeline — see ucv_pipeline.h.
 *
 * Threading: one capture thread does capture -> decode -> encode-submit, and
 * one drain thread does encode-drain -> transport-send.
 *
 * They are separate on purpose. MediaCodec's input and output sides run at
 * different rates, and draining inline would block capture whenever the
 * encoder was busy — which would distort the frame pacing that every jitter
 * and latency number in this experiment is measured against.
 */

#include "ucv_pipeline.h"
#include "ucv_encoder.h"
#include "ucv_transport.h"

#include <libuvc/libuvc.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <android/log.h>

#define TAG "ucv-pipeline"
extern void ucv_ring_log(const char *fmt, ...);
#define PLOG(...)                                                \
  do {                                                           \
    __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__);     \
    ucv_ring_log(__VA_ARGS__);                                   \
  } while (0)
#define PLOGE(...)                                               \
  do {                                                           \
    __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__);    \
    ucv_ring_log(__VA_ARGS__);                                   \
  } while (0)

/* Capture timestamps must survive the trip through MediaCodec, which only
 * carries a presentation timestamp. The PTS is used as the key into this
 * small ring so t_capture can be recovered when the encoded frame comes out.
 *
 * 64 entries is far more than MediaCodec's in-flight depth (typically < 8);
 * if an entry is ever evicted before use it means the encoder fell badly
 * behind, and that frame is dropped rather than sent with a wrong timestamp.
 */
#define PTS_RING 64
typedef struct {
  int64_t  pts_us;
  uint64_t t_capture_ns;
  int      used;
} pts_entry_t;

static pts_entry_t     g_pts[PTS_RING];
static pthread_mutex_t g_pts_lock = PTHREAD_MUTEX_INITIALIZER;

static void pts_put(int64_t pts_us, uint64_t t_capture_ns) {
  const int slot = (int)((uint64_t)pts_us % PTS_RING);
  pthread_mutex_lock(&g_pts_lock);
  g_pts[slot].pts_us = pts_us;
  g_pts[slot].t_capture_ns = t_capture_ns;
  g_pts[slot].used = 1;
  pthread_mutex_unlock(&g_pts_lock);
}

/* Returns 0 and fills *out on a hit; -1 if the entry was overwritten. */
static int pts_get(int64_t pts_us, uint64_t *out) {
  const int slot = (int)((uint64_t)pts_us % PTS_RING);
  int rc = -1;
  pthread_mutex_lock(&g_pts_lock);
  if (g_pts[slot].used && g_pts[slot].pts_us == pts_us) {
    *out = g_pts[slot].t_capture_ns;
    rc = 0;
  }
  pthread_mutex_unlock(&g_pts_lock);
  return rc;
}

static volatile int     g_quit = 1;
static int              g_running = 0;
static pthread_t        g_cap_th, g_drain_th;
static uvc_stream_handle_t *g_strmh = NULL;
static ucv_encoder_t   *g_enc = NULL;
static ucv_jpeg_decoder_t *g_dec = NULL;
static ucv_transport_t *g_tx = NULL;

static ucv_pipeline_stats_t g_stats;
static pthread_mutex_t      g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char                 g_enc_desc[256] = "(not started)";

/* Frame sequence is assigned at SEND time, not capture time, so it stays
 * contiguous on the wire. A gap the receiver sees is then unambiguously
 * network loss rather than a local encoder drop — two very different
 * findings that must not be conflated. */
static uint32_t g_send_seq = 0;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------- */
/* Capture thread: UVC -> JPEG decode -> encoder input               */
/* ---------------------------------------------------------------- */
static void *capture_thread(void *arg) {
  (void)arg;
  PLOG("pipeline: capture thread started");

  uint64_t dec_acc = 0, dec_n = 0;
  uint64_t capture_misses = 0;
  int64_t  pts_us = 0;

  while (!g_quit) {
    uvc_frame_t *fr = NULL;
    uvc_error_t res = uvc_stream_get_frame(g_strmh, &fr, 1000000 /* 1s */);
    if (res != UVC_SUCCESS || !fr || !fr->data) {
      capture_misses++;
      if (capture_misses == 1 || capture_misses == 5 ||
          capture_misses % 10 == 0) {
        PLOGE("pipeline: capture wait failed res=%d misses=%llu "
              "(no completed UVC frame)",
              (int)res, (unsigned long long)capture_misses);
      }
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
      continue;
    }

    capture_misses = 0;

    /* Stamped the instant the frame leaves USB — this is t_capture for the
     * whole measurement chain. */
    const uint64_t t_cap = now_ns();

    pthread_mutex_lock(&g_stats_lock);
    g_stats.frames_captured++;
    pthread_mutex_unlock(&g_stats_lock);

    /* One-shot markers so the on-screen log shows exactly how far the very
     * first frame got. Each stage failing silently looks identical from the
     * counters alone until they have been watched for a while. */
    if (g_stats.frames_captured == 1)
      PLOG("pipeline: FIRST FRAME CAPTURED (%zu bytes)", fr->data_bytes);

    const uint8_t *nv12 = NULL;
    size_t nv12_size = 0;
    const uint64_t t_dec0 = now_ns();
    if (ucv_jpeg_decode_to_nv12(g_dec, (const uint8_t *)fr->data,
                                fr->data_bytes, &nv12, &nv12_size) != 0) {
      pthread_mutex_lock(&g_stats_lock);
      g_stats.decode_errors++;
      const uint64_t n = g_stats.decode_errors;
      pthread_mutex_unlock(&g_stats_lock);
      if (n == 1)
        PLOGE("pipeline: FIRST DECODE FAILED - camera mode vs decoder mismatch?");
      continue; /* corrupt frame: counted, stream keeps running */
    }
    if (g_stats.frames_decoded == 0)
      PLOG("pipeline: FIRST FRAME DECODED -> %zu bytes NV12", nv12_size);
    const uint64_t t_dec1 = now_ns();
    dec_acc += (t_dec1 - t_dec0);
    dec_n++;

    /* MediaCodec uses PTS for rate control, so it must be monotonic. Derived
     * from the capture clock rather than a frame counter so that a dropped
     * frame does not make the encoder think the frame rate changed. */
    pts_us = (int64_t)(t_cap / 1000ull);
    pts_put(pts_us, t_cap);

    if (ucv_encoder_submit_nv12(g_enc, nv12, nv12_size, pts_us) == 0) {
      pthread_mutex_lock(&g_stats_lock);
      g_stats.frames_decoded++;
      if (dec_n)
        g_stats.avg_decode_us = (uint32_t)(dec_acc / dec_n / 1000);
      pthread_mutex_unlock(&g_stats_lock);
    }

    if (dec_n >= 300) { /* keep the rolling average recent */
      dec_acc = 0;
      dec_n = 0;
    }
  }

  PLOG("pipeline: capture thread exiting");
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Drain thread: encoder output -> transport                         */
/* ---------------------------------------------------------------- */
static void *drain_thread(void *arg) {
  (void)arg;
  PLOG("pipeline: drain thread started");

  uint64_t enc_acc = 0, enc_n = 0;

  while (!g_quit) {
    const uint8_t *data = NULL;
    size_t size = 0;
    int64_t pts_us = 0;
    int is_key = 0;

    /* 20 ms: long enough to avoid spinning, short enough that stop() is
     * responsive without needing a separate wakeup mechanism. */
    int rc = ucv_encoder_drain(g_enc, &data, &size, &pts_us, &is_key, 20000);
    if (rc <= 0)
      continue;

    const uint64_t t_enc = now_ns();

    uint64_t t_cap = 0;
    if (pts_get(pts_us, &t_cap) != 0) {
      /* The capture timestamp was evicted, so this frame's latency cannot be
       * measured. Sending it with a wrong t_capture would silently corrupt
       * the glass-to-glass numbers, so it is dropped and counted instead. */
      pthread_mutex_lock(&g_stats_lock);
      g_stats.encoder_drops++;
      pthread_mutex_unlock(&g_stats_lock);
      continue;
    }

    enc_acc += (t_enc - t_cap);
    enc_n++;

    ucv_encoded_frame_t f;
    f.data = data;
    f.size = size;
    f.seq = g_send_seq;
    f.t_capture_ns = t_cap;
    f.t_encoded_ns = t_enc;
    f.is_keyframe = is_key;

    const int sr = g_tx->send(g_tx, &f);

    /* Confirm the very first send explicitly. Without this the only evidence
     * that bytes ever left the phone is a counter the operator has to catch
     * mid-scroll, and "sent stayed at 0" is the single most common bring-up
     * failure. */
    if (g_stats.frames_sent == 0) {
      if (sr == 0)
        PLOG("pipeline: FIRST FRAME SENT (%zu bytes, key=%d)", size, is_key);
      else
        PLOGE("pipeline: first send failed rc=%d (%s)", sr,
              sr == -ENOTCONN ? "no peer/connection" : "socket error");
    }

    pthread_mutex_lock(&g_stats_lock);
    g_stats.frames_encoded++;
    if (sr == 0) {
      g_stats.frames_sent++;
      g_stats.bytes_payload += size;
      g_send_seq++;  /* only advanced on a real send: see g_send_seq comment */
    } else if (sr != -ENOTCONN) {
      g_stats.send_errors++;
    }
    g_stats.bytes_wire = g_tx->bytes_wire;
    if (enc_n)
      g_stats.avg_encode_us = (uint32_t)(enc_acc / enc_n / 1000);
    pthread_mutex_unlock(&g_stats_lock);

    if (enc_n >= 300) {
      enc_acc = 0;
      enc_n = 0;
    }
  }

  PLOG("pipeline: drain thread exiting");
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ---------------------------------------------------------------- */

int ucv_pipeline_start(void *strmh, ucv_protocol_t proto, const char *peer_cfg,
                       int width, int height, int fps) {
  if (g_running) {
    PLOGE("pipeline: already running");
    return -EBUSY;
  }
  if (!strmh) {
    PLOGE("pipeline: no UVC stream");
    return -EINVAL;
  }

  g_tx = ucv_transport_get(proto);
  if (!g_tx) {
    PLOGE("pipeline: protocol %d is not implemented in this build. Refusing "
          "to run rather than silently using a different transport.",
          (int)proto);
    return -ENOSYS;
  }

  g_dec = ucv_jpeg_decoder_create(width, height);
  if (!g_dec) {
    PLOGE("pipeline: jpeg decoder init failed (%dx%d)", width, height);
    return -EINVAL;
  }

  ucv_encoder_config_t cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.fps = fps;
  cfg.bitrate_bps = UCV_ENC_DEFAULT_BITRATE;
  cfg.keyframe_interval_s = UCV_ENC_DEFAULT_KEYINT_S;
  cfg.require_hardware = 1; /* harness spec section 2: software run is invalid */

  g_enc = ucv_encoder_create(&cfg);
  if (!g_enc) {
    PLOGE("pipeline: encoder init failed");
    ucv_jpeg_decoder_destroy(g_dec);
    g_dec = NULL;
    return -EIO;
  }

  ucv_encoder_info_t info;
  ucv_encoder_get_info(g_enc, &info);
  snprintf(g_enc_desc, sizeof(g_enc_desc),
           "%s %dx%d@%d %dbps keyint=%ds hw=%s",
           info.codec_name[0] ? info.codec_name : "(name unavailable)",
           info.width, info.height, info.fps, info.bitrate_bps,
           info.keyframe_interval_s,
           info.hardware_accelerated == 1
               ? "yes"
               : (info.hardware_accelerated == 0 ? "NO" : "undetermined"));

  /* Echo the peer back: a typo in the PC address produces a run where the
   * receiver simply waits and no frames arrive, with no error on either
   * side. Showing what was actually parsed makes that visible immediately. */
  PLOG("pipeline: transport=%s peer=%s", g_tx->name,
       (peer_cfg && *peer_cfg) ? peer_cfg : "(empty!)");

  int rc = g_tx->start(g_tx, peer_cfg);
  if (rc != 0) {
    PLOGE("pipeline: transport '%s' start failed: %d", g_tx->name, rc);
    ucv_encoder_destroy(g_enc);
    g_enc = NULL;
    ucv_jpeg_decoder_destroy(g_dec);
    g_dec = NULL;
    return rc;
  }

  memset(&g_stats, 0, sizeof(g_stats));
  memset(g_pts, 0, sizeof(g_pts));
  g_send_seq = 0;
  g_tx->frames_sent = g_tx->bytes_payload = g_tx->bytes_wire =
      g_tx->send_errors = 0;

  g_strmh = (uvc_stream_handle_t *)strmh;
  g_quit = 0;

  if (pthread_create(&g_cap_th, NULL, capture_thread, NULL) != 0) {
    g_quit = 1;
    g_tx->stop(g_tx);
    ucv_encoder_destroy(g_enc);
    g_enc = NULL;
    ucv_jpeg_decoder_destroy(g_dec);
    g_dec = NULL;
    return -EAGAIN;
  }
  if (pthread_create(&g_drain_th, NULL, drain_thread, NULL) != 0) {
    g_quit = 1;
    pthread_join(g_cap_th, NULL);
    g_tx->stop(g_tx);
    ucv_encoder_destroy(g_enc);
    g_enc = NULL;
    ucv_jpeg_decoder_destroy(g_dec);
    g_dec = NULL;
    return -EAGAIN;
  }

  g_running = 1;
  PLOG("pipeline: %s -> %s", g_enc_desc, g_tx->name);
  return 0;
}

void ucv_pipeline_stop(void) {
  if (!g_running)
    return;
  g_quit = 1;
  pthread_join(g_cap_th, NULL);
  pthread_join(g_drain_th, NULL);

  if (g_tx)
    g_tx->stop(g_tx);
  if (g_enc) {
    ucv_encoder_destroy(g_enc);
    g_enc = NULL;
  }
  if (g_dec) {
    ucv_jpeg_decoder_destroy(g_dec);
    g_dec = NULL;
  }
  g_strmh = NULL;
  g_running = 0;

  PLOG("pipeline: stopped (cap=%llu dec=%llu enc=%llu sent=%llu "
       "decerr=%llu drop=%llu senderr=%llu)",
       (unsigned long long)g_stats.frames_captured,
       (unsigned long long)g_stats.frames_decoded,
       (unsigned long long)g_stats.frames_encoded,
       (unsigned long long)g_stats.frames_sent,
       (unsigned long long)g_stats.decode_errors,
       (unsigned long long)g_stats.encoder_drops,
       (unsigned long long)g_stats.send_errors);
}

int ucv_pipeline_running(void) { return g_running; }

void ucv_pipeline_get_stats(ucv_pipeline_stats_t *out) {
  if (!out)
    return;
  pthread_mutex_lock(&g_stats_lock);
  *out = g_stats;
  if (g_enc)
    out->encoder_drops += ucv_encoder_drops(g_enc);
  pthread_mutex_unlock(&g_stats_lock);
}

const char *ucv_pipeline_encoder_desc(void) { return g_enc_desc; }
