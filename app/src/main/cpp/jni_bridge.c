#include <jni.h>
#include <android/log.h>
#include <libusb.h>
#include <libuvc/libuvc.h>

#include "ucv_transport.h"
#include "ucv_control.h"
#include "ucv_pipeline.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TAG "uvcserver-native"

/* ------------------------------------------------------------------ */
/* In-app log ring buffer (mirrored to logcat) — the phone's USB port  */
/* is occupied by the camera, so `adb logcat` over USB isn't an        */
/* option while streaming; the UI polls getLogText() instead.          */
/* ------------------------------------------------------------------ */
#define LOG_LINES 200
#define LOG_LINE_LEN 200
static char g_log[LOG_LINES][LOG_LINE_LEN];
static int g_log_head = 0;
static int g_log_count = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ring_log(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_log_mutex);
  strncpy(g_log[g_log_head], line, LOG_LINE_LEN - 1);
  g_log[g_log_head][LOG_LINE_LEN - 1] = '\0';
  g_log_head = (g_log_head + 1) % LOG_LINES;
  if (g_log_count < LOG_LINES)
    g_log_count++;
  pthread_mutex_unlock(&g_log_mutex);
}

/* Exported so the encoder and transports can reach the on-screen log too —
 * with the USB port taken by the camera, that ring buffer is the only debug
 * surface available while streaming. */
void ucv_ring_log(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  ring_log("%s", line);
}

#define LOGI(...)                                                   \
  do {                                                              \
    __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__);        \
    ring_log(__VA_ARGS__);                                          \
  } while (0)
#define LOGE(...)                                                   \
  do {                                                              \
    __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__);       \
    ring_log(__VA_ARGS__);                                          \
  } while (0)

static uvc_context_t *g_ctx = NULL;
static uvc_device_handle_t *g_devh = NULL;
static int g_libusb_opts_set = 0;

/* ------------------------------------------------------------------ */
/* Mode selection (same scoring as the Windows mjpegserver)            */
/* ------------------------------------------------------------------ */
typedef struct {
  enum uvc_frame_format format;
  const uvc_frame_desc_t *frame;
  int fps;
  uint64_t score;
} mode_candidate_t;

static int cmp_candidates(const void *a, const void *b) {
  const mode_candidate_t *ca = a, *cb = b;
  if (ca->score < cb->score)
    return 1;
  if (ca->score > cb->score)
    return -1;
  return 0;
}

static int collect_modes(uvc_device_handle_t *devh, mode_candidate_t *cands,
                         int max) {
  int nc = 0;
  const uvc_format_desc_t *fmt;
  for (fmt = uvc_get_format_descs(devh); fmt && nc < max; fmt = fmt->next) {
    enum uvc_frame_format ff;
    switch (fmt->bDescriptorSubtype) {
      case UVC_VS_FORMAT_MJPEG:
        ff = UVC_FRAME_FORMAT_MJPEG;
        break;
      default:
        continue; /* this app only serves MJPEG, no YUYV path */
    }
    for (const uvc_frame_desc_t *f = fmt->frame_descs; f; f = f->next) {
      if (!f->wWidth || !f->wHeight)
        continue;
      /* Discrete interval list: one candidate per offered fps (not just the
       * fastest) so callers can ask for an exact width/height/fps match —
       * e.g. a sensor's high-res mode capping at 20fps instead of 30. */
      if (f->bFrameIntervalType > 0 && f->intervals) {
        for (int ivi = 0; ivi < f->bFrameIntervalType && nc < max; ivi++) {
          uint32_t interval = f->intervals[ivi];
          if (!interval)
            continue;
          int fps = 10000000 / interval;
          if (fps > 60)
            fps = 60;
          cands[nc].format = ff;
          cands[nc].frame = f;
          cands[nc].fps = fps;
          cands[nc].score = (uint64_t)f->wWidth * f->wHeight * fps;
          nc++;
        }
        continue;
      }
      uint32_t interval = f->dwDefaultFrameInterval;
      if (f->dwMinFrameInterval)
        interval = f->dwMinFrameInterval;
      if (!interval)
        continue;
      int fps = 10000000 / interval;
      if (fps > 60)
        fps = 60;
      cands[nc].format = ff;
      cands[nc].frame = f;
      cands[nc].fps = fps;
      cands[nc].score = (uint64_t)f->wWidth * f->wHeight * fps;
      nc++;
    }
  }
  qsort(cands, nc, sizeof(*cands), cmp_candidates);
  return nc;
}

/* cands is sorted best-score-first (see cmp_candidates), so index 0 is the
 * "auto-best" (usually highest resolution) mode. High-resolution MJPEG is
 * routinely unreliable over isochronous USB (dropped/corrupt packets show up
 * as garbled JPEG frames) — capW/capH/capFps (any may be 0 = don't care) let
 * a caller pin a known-good mode instead of always getting auto-best.
 * Returns the cands[] index to start the uvc_get_stream_ctrl_format_size
 * negotiation loop from. */
static int find_mode_start_index(const mode_candidate_t *cands, int nc,
                                 int capW, int capH, int capFps) {
  if (capW <= 0 || capH <= 0)
    return 0;
  int exact_wh_fps = -1, exact_wh_any_fps = -1;
  for (int i = 0; i < nc; i++) {
    if (cands[i].frame->wWidth != capW || cands[i].frame->wHeight != capH)
      continue;
    if (exact_wh_any_fps < 0)
      exact_wh_any_fps = i;
    if (capFps > 0 && cands[i].fps == capFps) {
      exact_wh_fps = i;
      break;
    }
  }
  if (exact_wh_fps >= 0)
    return exact_wh_fps;
  if (exact_wh_any_fps >= 0)
    return exact_wh_any_fps;
  LOGE("no %dx%d mode found, falling back to auto-best", capW, capH);
  return 0;
}

/* ------------------------------------------------------------------ */
/* MJPEG passthrough HTTP server — raw camera frames, no decode/re-    */
/* encode, served as multipart/x-mixed-replace so any browser or       */
/* <img> tag can view the stream directly.                             */
/* ------------------------------------------------------------------ */
typedef struct {
  uint8_t *data;
  size_t len, cap;
  uint64_t seq;
  pthread_mutex_t lock;
  pthread_cond_t cond;
} frame_state_t;

static frame_state_t g_state = {
    .data = NULL,
    .len = 0,
    .cap = 0,
    .seq = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static volatile int g_quit = 1; /* 1 = server not running */
static uvc_stream_handle_t *g_strmh = NULL;

static int start_experiment_native(int protocol, const char *peer_cfg,
                                   int capW, int capH, int capFps);
static void stop_experiment_native(void);
static int remote_start(const char *peer_ip,
                        const ucv_start_run_payload_t *request);
static void remote_stop(void);
static int remote_get_mode(int index, uint16_t *width, uint16_t *height,
                           uint8_t *fps);
static int g_lsock = -1;
static pthread_t g_cap_thread, g_accept_thread;
static int g_server_running = 0;

static int send_all(int s, const uint8_t *buf, int len) {
  int off = 0;
  while (off < len) {
    ssize_t n = send(s, buf + off, (size_t)(len - off), 0);
    if (n <= 0)
      return -1;
    off += (int)n;
  }
  return 0;
}

static void deadline_ms(struct timespec *ts, int ms) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_nsec += (long)ms * 1000000L;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec += 1;
  }
}

static void *capture_thread(void *arg) {
  uvc_stream_handle_t *strmh = (uvc_stream_handle_t *)arg;
  LOGI("capture thread started");
  while (!g_quit) {
    uvc_frame_t *fr = NULL;
    uvc_error_t res = uvc_stream_get_frame(strmh, &fr, -1);
    if (res == UVC_SUCCESS && fr && fr->data) {
      pthread_mutex_lock(&g_state.lock);
      if (g_state.cap < fr->data_bytes) {
        free(g_state.data);
        g_state.data = (uint8_t *)malloc(fr->data_bytes);
        g_state.cap = fr->data_bytes;
      }
      memcpy(g_state.data, fr->data, fr->data_bytes);
      g_state.len = fr->data_bytes;
      g_state.seq++;
      pthread_mutex_unlock(&g_state.lock);
      pthread_cond_broadcast(&g_state.cond);
    } else {
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }
  }
  LOGI("capture thread exiting");
  return NULL;
}

typedef struct {
  int fd;
} client_arg_t;

static void *client_thread(void *arg) {
  int s = ((client_arg_t *)arg)->fd;
  free(arg);

  struct timeval tmo = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));

  static const char hdr[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache, no-store\r\n"
      "Connection: close\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n";
  if (send_all(s, (const uint8_t *)hdr, (int)sizeof(hdr) - 1) < 0) {
    close(s);
    return NULL;
  }

  uint8_t *buf = NULL;
  size_t cap = 0;
  uint64_t last_seq = 0;
  while (!g_quit) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.seq == last_seq && !g_quit) {
      struct timespec ts;
      deadline_ms(&ts, 100);
      pthread_cond_timedwait(&g_state.cond, &g_state.lock, &ts);
    }
    size_t n = g_state.len;
    if (n && g_state.seq != last_seq) {
      last_seq = g_state.seq;
      if (cap < n) {
        free(buf);
        buf = (uint8_t *)malloc(n);
        cap = n;
      }
      memcpy(buf, g_state.data, n);
    } else {
      n = 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (!n)
      continue;

    char b[256];
    int bn = snprintf(b, sizeof(b),
                      "--frame\r\nContent-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      n);
    if (send_all(s, (const uint8_t *)b, bn) < 0)
      break;
    if (send_all(s, buf, (int)n) < 0)
      break;
    if (send_all(s, (const uint8_t *)"\r\n", 2) < 0)
      break;
  }

  free(buf);
  close(s);
  return NULL;
}

static void *accept_thread(void *arg) {
  (void)arg;
  LOGI("accept thread started");
  while (!g_quit) {
    int cs = accept(g_lsock, NULL, NULL);
    if (cs < 0) {
      if (g_quit)
        break;
      continue;
    }
    client_arg_t *ca = (client_arg_t *)malloc(sizeof(client_arg_t));
    ca->fd = cs;
    pthread_t th;
    if (pthread_create(&th, NULL, client_thread, ca) == 0) {
      pthread_detach(th);
    } else {
      close(cs);
      free(ca);
    }
  }
  LOGI("accept thread exiting");
  return NULL;
}

/* Clears any leftover frame from a previous server session so a freshly
 * started server never serves a stale frame. */
static void reset_frame_state(void) {
  pthread_mutex_lock(&g_state.lock);
  free(g_state.data);
  g_state.data = NULL;
  g_state.len = 0;
  g_state.cap = 0;
  g_state.seq = 0;
  pthread_mutex_unlock(&g_state.lock);
}

static void stop_server_locked(void) {
  if (!g_server_running)
    return;
  g_quit = 1;
  if (g_lsock >= 0) {
    shutdown(g_lsock, SHUT_RDWR);
    close(g_lsock);
    g_lsock = -1;
  }
  pthread_join(g_cap_thread, NULL);
  pthread_join(g_accept_thread, NULL);
  if (g_strmh) {
    uvc_stream_stop(g_strmh);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
  }
  g_server_running = 0;
  LOGI("server stopped");
}

/* ------------------------------------------------------------------ */
/* JNI exports                                                         */
/* ------------------------------------------------------------------ */

JNIEXPORT jboolean JNICALL
Java_com_anjas_uvcserver_UvcNative_openDevice(JNIEnv *env, jobject thiz, jint fd) {
  (void)env;
  (void)thiz;
  uvc_error_t res;

  if (g_devh) {
    LOGI("openDevice: already open, closing previous handle first");
    uvc_close(g_devh);
    g_devh = NULL;
  }
  if (!g_libusb_opts_set) {
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
    g_libusb_opts_set = 1;
  }
  if (!g_ctx) {
    res = uvc_init(&g_ctx, NULL);
    if (res < 0) {
      LOGE("uvc_init failed: %d", res);
      return JNI_FALSE;
    }
  }

  res = uvc_wrap((int)fd, g_ctx, &g_devh);
  if (res < 0) {
    LOGE("uvc_wrap failed: %d", res);
    g_devh = NULL;
    return JNI_FALSE;
  }

  uvc_device_t *dev = uvc_get_device(g_devh);
  uvc_device_descriptor_t *desc = NULL;
  if (uvc_get_device_descriptor(dev, &desc) == UVC_SUCCESS && desc) {
    LOGI("Opened UVC device: vid=0x%04x pid=0x%04x mfr=%s product=%s",
         desc->idVendor, desc->idProduct,
         desc->manufacturer ? desc->manufacturer : "?",
         desc->product ? desc->product : "?");
    uvc_free_device_descriptor(desc);
  } else {
    LOGI("Opened UVC device (descriptor unavailable)");
  }

  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_anjas_uvcserver_UvcNative_closeDevice(JNIEnv *env, jobject thiz) {
  (void)env;
  (void)thiz;
  stop_server_locked();
  if (g_devh) {
    uvc_close(g_devh);
    g_devh = NULL;
    LOGI("Device closed");
  }
  if (g_ctx) {
    uvc_exit(g_ctx);
    g_ctx = NULL;
  }
}

JNIEXPORT jboolean JNICALL
Java_com_anjas_uvcserver_UvcNative_startServer(JNIEnv *env, jobject thiz,
                                               jint port, jint capW,
                                               jint capH, jint capFps) {
  (void)env;
  (void)thiz;
  if (!g_devh) {
    LOGE("startServer: no device open");
    return JNI_FALSE;
  }
  if (g_server_running) {
    LOGE("startServer: already running");
    return JNI_FALSE;
  }
  reset_frame_state();

  mode_candidate_t cands[256];
  int nc = collect_modes(g_devh, cands, 256);
  if (!nc) {
    LOGE("No MJPEG streaming mode found");
    return JNI_FALSE;
  }
  for (int i = 0; i < nc; i++) {
    LOGI("mode[%d]: %dx%d@%dfps", i, cands[i].frame->wWidth,
         cands[i].frame->wHeight, cands[i].fps);
  }

  int idx = find_mode_start_index(cands, nc, capW, capH, capFps);

  uvc_stream_ctrl_t ctrl;
  int chosen = -1;
  for (int i = idx; i < nc; i++) {
    uvc_error_t r = uvc_get_stream_ctrl_format_size(
        g_devh, &ctrl, cands[i].format, cands[i].frame->wWidth,
        cands[i].frame->wHeight, cands[i].fps);
    if (r == UVC_SUCCESS) {
      chosen = i;
      break;
    }
  }
  if (chosen < 0) {
    LOGE("Camera rejected every streaming mode");
    return JNI_FALSE;
  }

  uvc_error_t res = uvc_stream_open_ctrl(g_devh, &g_strmh, &ctrl);
  if (res < 0) {
    LOGE("uvc_stream_open_ctrl failed: %d", res);
    return JNI_FALSE;
  }
  res = uvc_stream_start(g_strmh, NULL, NULL, 0);
  if (res < 0) {
    LOGE("uvc_stream_start failed: %d", res);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
    return JNI_FALSE;
  }

  g_lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_lsock < 0) {
    LOGE("socket() failed: errno=%d", errno);
    uvc_stream_stop(g_strmh);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
    return JNI_FALSE;
  }
  int one = 1;
  setsockopt(g_lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(g_lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    LOGE("bind failed on port %d: errno=%d", port, errno);
    close(g_lsock);
    g_lsock = -1;
    uvc_stream_stop(g_strmh);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
    return JNI_FALSE;
  }
  listen(g_lsock, 8);

  g_quit = 0;
  pthread_create(&g_cap_thread, NULL, capture_thread, g_strmh);
  pthread_create(&g_accept_thread, NULL, accept_thread, NULL);
  g_server_running = 1;

  LOGI("MJPEG server on :%d  mode=%dx%d@%dfps", port,
       cands[chosen].frame->wWidth, cands[chosen].frame->wHeight,
       cands[chosen].fps);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_anjas_uvcserver_UvcNative_stopServer(JNIEnv *env, jobject thiz) {
  (void)env;
  (void)thiz;
  stop_server_locked();
}

/* ------------------------------------------------------------------ */
/* Protocol experiment JNI surface                                     */
/* ------------------------------------------------------------------ */
/* These drive the measurement harness (see experiment/docs/). They are
 * separate from the plain MJPEG server above so the existing viewer path
 * keeps working untouched while an experiment run is being set up. */

/* Starts the upstream control listener (PC -> phone commands + the PING
 * responder the PC needs for clock synchronisation). Without this running,
 * the receiver refuses to start a run at all — deliberately, because every
 * one-way latency number depends on the clock offset it provides. */
JNIEXPORT jboolean JNICALL
Java_com_anjas_uvcserver_UvcNative_startControl(JNIEnv *env, jobject thiz,
                                                jint port) {
  (void)env;
  (void)thiz;
  ucv_control_set_callbacks(remote_start, remote_stop, remote_get_mode);
  int rc = ucv_control_start(port);
  if (rc != 0) {
    LOGE("startControl failed: %d", rc);
    return JNI_FALSE;
  }
  LOGI("control channel listening on :%d", port > 0 ? port : 8200);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_anjas_uvcserver_UvcNative_stopControl(JNIEnv *env, jobject thiz) {
  (void)env;
  (void)thiz;
  ucv_control_stop();
  LOGI("control channel stopped");
}

/* Current view parameters as driven from the PC, formatted for the on-screen
 * log — the phone's USB port is taken by the camera, so this is the only way
 * to confirm on-device that upstream commands are actually landing. */
JNIEXPORT jstring JNICALL
Java_com_anjas_uvcserver_UvcNative_getControlState(JNIEnv *env, jobject thiz) {
  (void)thiz;
  ucv_view_state_t v;
  ucv_control_stats_t s;
  ucv_control_get_view(&v);
  ucv_control_get_stats(&s);

  char buf[256];
  snprintf(buf, sizeof(buf),
           "alpha=%.3f beta=%.3f zoom=%.3f preset=%u | rx=%llu crc_drop=%llu "
           "reorder=%llu",
           v.alpha, v.beta, v.zoom, (unsigned)v.preset,
           (unsigned long long)s.cmds_received,
           (unsigned long long)s.cmds_dropped_crc,
           (unsigned long long)s.reorder_events);
  return (*env)->NewStringUTF(env, buf);
}

/* Sets the run identifier every frame is stamped with, so the receiver can
 * discard frames leaking in from a previous run instead of averaging them
 * into this one's statistics. */
JNIEXPORT void JNICALL
Java_com_anjas_uvcserver_UvcNative_setRunId(JNIEnv *env, jobject thiz,
                                            jstring runId) {
  (void)thiz;
  const char *s = (*env)->GetStringUTFChars(env, runId, NULL);
  if (!s)
    return;
  ucv_set_run_id(s);
  LOGI("run_id set: %s", s);
  (*env)->ReleaseStringUTFChars(env, runId, s);
}

/* Starts a measurement run: negotiates the pinned UVC mode, then hands the
 * stream to the shared pipeline (decode -> H.264 -> transport).
 *
 * Separate from startServer() above so the plain MJPEG viewer path keeps
 * working untouched while an experiment is being set up. */
static int start_experiment_native(int protocol, const char *peer_cfg,
                                   int capW, int capH, int capFps) {
  if (!g_devh) {
    LOGE("startExperiment: no device open");
    return -ENODEV;
  }
  if (g_server_running) {
    LOGE("startExperiment: stop the MJPEG server first");
    return -EBUSY;
  }
  if (ucv_pipeline_running()) {
    LOGE("startExperiment: already running");
    return -EBUSY;
  }

  mode_candidate_t cands[256];
  int nc = collect_modes(g_devh, cands, 256);
  if (!nc) {
    LOGE("No MJPEG streaming mode found");
    return -ENOTSUP;
  }
  for (int i = 0; i < nc; i++) {
    LOGI("mode[%d]: %dx%d@%dfps", i, cands[i].frame->wWidth,
         cands[i].frame->wHeight, cands[i].fps);
  }

  int idx = find_mode_start_index(cands, nc, capW, capH, capFps);

  uvc_stream_ctrl_t ctrl;
  int chosen = -1;
  for (int i = idx; i < nc; i++) {
    if (uvc_get_stream_ctrl_format_size(g_devh, &ctrl, cands[i].format,
                                        cands[i].frame->wWidth,
                                        cands[i].frame->wHeight,
                                        cands[i].fps) == UVC_SUCCESS) {
      chosen = i;
      break;
    }
  }
  if (chosen < 0) {
    LOGE("Camera rejected every streaming mode");
    return -EINVAL;
  }

  uvc_error_t res = uvc_stream_open_ctrl(g_devh, &g_strmh, &ctrl);
  if (res < 0) {
    LOGE("uvc_stream_open_ctrl failed: %d", res);
    return -EIO;
  }
  res = uvc_stream_start(g_strmh, NULL, NULL, 0);
  if (res < 0) {
    LOGE("uvc_stream_start failed: %d", res);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
    return -EIO;
  }

  /* The encoder is configured for the mode the camera actually gave us, not
   * the one we asked for — a silent mismatch would make every frame fail to
   * decode. */
  const int w = cands[chosen].frame->wWidth;
  const int h = cands[chosen].frame->wHeight;
  const int fps = cands[chosen].fps;

  int rc = ucv_pipeline_start(g_strmh, (ucv_protocol_t)protocol,
                              peer_cfg ? peer_cfg : "", w, h, fps);

  if (rc != 0) {
    /* The on-screen log is the only debug surface during a run (the USB port
     * is taken by the camera), so translate the errno rather than leaving the
     * operator to decode a negative number. */
    const char *why;
    switch (-rc) {
      case ENOSYS:
        why = "protocol not implemented in this build";
        break;
      case EIO:
        why = "encoder init failed - no HARDWARE H.264 encoder, "
              "or one was rejected as software";
        break;
      case EINVAL:
        why = "jpeg decoder rejected the camera mode (odd width/height?)";
        break;
      case EBUSY:
        why = "pipeline already running";
        break;
      case EAGAIN:
        why = "could not spawn pipeline threads";
        break;
      default:
        why = "transport start failed - check the PC IP entered above";
        break;
    }
    LOGE("START FAILED (%d): %s", rc, why);
    uvc_stream_stop(g_strmh);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
    return rc;
  }

  LOGI("experiment running: %dx%d@%dfps proto=%d", w, h, fps, (int)protocol);
  return 0;
}

JNIEXPORT jboolean JNICALL
Java_com_anjas_uvcserver_UvcNative_startExperiment(JNIEnv *env, jobject thiz,
                                                   jint protocol,
                                                   jstring peerCfg, jint capW,
                                                   jint capH, jint capFps) {
  (void)thiz;
  const char *cfg = (*env)->GetStringUTFChars(env, peerCfg, NULL);
  int rc = start_experiment_native(protocol, cfg ? cfg : "", capW, capH, capFps);
  if (cfg)
    (*env)->ReleaseStringUTFChars(env, peerCfg, cfg);
  return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

static void stop_experiment_native(void) {
  ucv_pipeline_stop();
  if (g_strmh) {
    uvc_stream_stop(g_strmh);
    uvc_stream_close(g_strmh);
    g_strmh = NULL;
  }
}

static int remote_start(const char *peer_ip,
                        const ucv_start_run_payload_t *request) {
  if (!request || request->protocol_id != UCV_PROTO_RAWUDP ||
      !request->width || !request->height || !request->fps)
    return -EINVAL;
  stop_experiment_native();
  char peer[64];
  snprintf(peer, sizeof(peer), "%s:%u", peer_ip,
           request->video_port ? request->video_port : UCV_PORT_RAWUDP);
  LOGI("remote START: %ux%u@%u -> %s", request->width, request->height,
       request->fps, peer);
  return start_experiment_native(request->protocol_id, peer, request->width,
                                 request->height, request->fps);
}

static void remote_stop(void) {
  LOGI("remote STOP");
  stop_experiment_native();
}

static int remote_get_mode(int index, uint16_t *width, uint16_t *height,
                           uint8_t *fps) {
  if (!g_devh || index < 0 || index >= 256)
    return -EINVAL;
  mode_candidate_t cands[256];
  int count = collect_modes(g_devh, cands, 256);
  if (index >= count)
    return -ENOENT;
  *width = cands[index].frame->wWidth;
  *height = cands[index].frame->wHeight;
  *fps = (uint8_t)cands[index].fps;
  return 0;
}

JNIEXPORT void JNICALL
Java_com_anjas_uvcserver_UvcNative_stopExperiment(JNIEnv *env, jobject thiz) {
  (void)env;
  (void)thiz;
  stop_experiment_native();
}

JNIEXPORT jboolean JNICALL
Java_com_anjas_uvcserver_UvcNative_isExperimentRunning(JNIEnv *env,
                                                       jobject thiz) {
  (void)env;
  (void)thiz;
  return ucv_pipeline_running() ? JNI_TRUE : JNI_FALSE;
}

/* Live pipeline health for the on-screen log — the only way to confirm the
 * rig is working without a PC attached. */
JNIEXPORT jstring JNICALL
Java_com_anjas_uvcserver_UvcNative_getExperimentState(JNIEnv *env,
                                                      jobject thiz) {
  (void)thiz;
  ucv_pipeline_stats_t s;
  ucv_pipeline_get_stats(&s);

  char buf[512];
  snprintf(buf, sizeof(buf),
           "%s\ncap=%llu dec=%llu enc=%llu sent=%llu\n"
           "decerr=%llu drop=%llu senderr=%llu\n"
           "decode=%ums encode=%ums payload=%lluKB",
           ucv_pipeline_encoder_desc(),
           (unsigned long long)s.frames_captured,
           (unsigned long long)s.frames_decoded,
           (unsigned long long)s.frames_encoded,
           (unsigned long long)s.frames_sent,
           (unsigned long long)s.decode_errors,
           (unsigned long long)s.encoder_drops,
           (unsigned long long)s.send_errors,
           s.avg_decode_us / 1000, s.avg_encode_us / 1000,
           (unsigned long long)(s.bytes_payload / 1024));
  return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_anjas_uvcserver_UvcNative_getLogText(JNIEnv *env, jobject thiz) {
  (void)thiz;
  pthread_mutex_lock(&g_log_mutex);
  char buf[LOG_LINES * LOG_LINE_LEN];
  buf[0] = '\0';
  int start = (g_log_count < LOG_LINES) ? 0 : g_log_head;
  for (int i = 0; i < g_log_count; i++) {
    int idx = (start + i) % LOG_LINES;
    strncat(buf, g_log[idx], sizeof(buf) - strlen(buf) - 2);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
  }
  pthread_mutex_unlock(&g_log_mutex);
  return (*env)->NewStringUTF(env, buf);
}
