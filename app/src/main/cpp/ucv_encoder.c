/* MediaCodec H.264 encoder + libjpeg-turbo MJPEG->NV12 decode.
 *
 * See ucv_encoder.h for the interface contract and why this lives in native
 * code rather than Kotlin.
 */

#include "ucv_encoder.h"

#ifndef UCV_JPEG_ONLY
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>
#include <setjmp.h>

#ifndef UCV_JPEG_ONLY

#define TAG "ucv-encoder"
#define ELOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ELOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Callers outside this file (jni_bridge.c) mirror encoder messages into the
 * on-screen ring buffer, since the USB port is taken by the camera and
 * logcat is unavailable while streaming. */
extern void ucv_ring_log(const char *fmt, ...);
#define ELOG_BOTH(...)          \
  do {                          \
    ELOGI(__VA_ARGS__);         \
    ucv_ring_log(__VA_ARGS__);  \
  } while (0)
#define ELOGE_BOTH(...)         \
  do {                          \
    ELOGE(__VA_ARGS__);         \
    ucv_ring_log(__VA_ARGS__);  \
  } while (0)

/* MediaCodec colour format constant (MediaCodecInfo.CodecCapabilities).
 * Not exposed as a symbol by the NDK headers, so it is defined here. */
#define COLOR_FormatYUV420SemiPlanar 21

/* AMediaCodec_getName / _releaseName are API 28+, but minSdk here is 24.
 * Resolved dynamically so the app still runs on 24-27 — where the hardware
 * check is reported as "undetermined" rather than silently assumed to pass. */
typedef media_status_t (*getname_fn)(AMediaCodec *, char **);
typedef void (*releasename_fn)(AMediaCodec *, char *);

struct ucv_encoder {
  AMediaCodec *codec;
  ucv_encoder_info_t info;

  /* SPS/PPS from BUFFER_FLAG_CODEC_CONFIG. Prepended to every keyframe so a
   * late-joining receiver and every independently published HLS segment get a
   * self-contained Annex-B random-access point. */
  uint8_t *csd;
  size_t   csd_size;

  /* Output staging: AMediaCodec requires the output buffer be released
   * promptly, so the access unit is copied out before releasing rather than
   * handing the caller a buffer the codec still owns. */
  uint8_t *out_buf;
  size_t   out_cap;

  /* One input buffer may be held while libjpeg writes NV12 into it. */
  ssize_t  input_index;
  size_t   input_capacity;

  uint64_t frames_in;
  uint64_t frames_out;
  uint64_t drops;
};

/* ---------------------------------------------------------------- */
/* Hardware-codec verification                                       */
/* ---------------------------------------------------------------- */
/* Software encoders are named "c2.android.*" or "OMX.google.*" by
 * convention. A software run must be rejected, not footnoted: it changes the
 * encode cost baked into every latency number in the comparison. */
static int codec_name_is_hardware(const char *name) {
  if (!name || !*name)
    return -1;
  if (strstr(name, "c2.android.") == name)
    return 0;
  if (strstr(name, "OMX.google.") == name)
    return 0;
  return 1;
}

static void query_codec_name(AMediaCodec *codec, ucv_encoder_info_t *info) {
  info->codec_name[0] = '\0';
  info->hardware_accelerated = -1;

  /* libmediandk is already loaded (we linked it); RTLD_DEFAULT finds the
   * symbol when the platform is new enough and returns NULL when it is not. */
  getname_fn get_name = (getname_fn)dlsym(RTLD_DEFAULT, "AMediaCodec_getName");
  releasename_fn rel_name =
      (releasename_fn)dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName");

  if (!get_name || !rel_name) {
    ELOG_BOTH("codec name unavailable (needs API 28, device is %d)",
              android_get_device_api_level());
    return;
  }

  char *name = NULL;
  if (get_name(codec, &name) == AMEDIA_OK && name) {
    snprintf(info->codec_name, sizeof(info->codec_name), "%s", name);
    info->hardware_accelerated = codec_name_is_hardware(name);
    rel_name(codec, name);
  }
}

/* ---------------------------------------------------------------- */
/* Encoder lifecycle                                                 */
/* ---------------------------------------------------------------- */

ucv_encoder_t *ucv_encoder_create(const ucv_encoder_config_t *cfg) {
  ucv_encoder_config_t c;
  if (cfg) {
    c = *cfg;
  } else {
    c.width = UCV_ENC_DEFAULT_WIDTH;
    c.height = UCV_ENC_DEFAULT_HEIGHT;
    c.fps = UCV_ENC_DEFAULT_FPS;
    c.bitrate_bps = UCV_ENC_DEFAULT_BITRATE;
    c.keyframe_interval_s = UCV_ENC_DEFAULT_KEYINT_S;
    c.require_hardware = 1;
  }

  ucv_encoder_t *e = (ucv_encoder_t *)calloc(1, sizeof(*e));
  if (!e)
    return NULL;
  e->input_index = -1;

  e->codec = AMediaCodec_createEncoderByType("video/avc");
  if (!e->codec) {
    ELOGE_BOTH("AMediaCodec_createEncoderByType(video/avc) failed");
    free(e);
    return NULL;
  }

  AMediaFormat *fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, c.width);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, c.height);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, c.fps);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, c.bitrate_bps);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                        c.keyframe_interval_s);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        COLOR_FormatYUV420SemiPlanar);
  /* Profile/level/bitrate-mode keys are API 28+. Set by string key so the
   * values still reach the codec on 24-27, where the named constants do not
   * exist. Ignored by codecs that do not honour them; the negotiated result
   * is reported either way so a run can be validated. */
  AMediaFormat_setInt32(fmt, "profile", 1);        /* AVCProfileBaseline */
  AMediaFormat_setInt32(fmt, "level", 512);        /* AVCLevel31 */
  AMediaFormat_setInt32(fmt, "bitrate-mode", 2);   /* BITRATE_MODE_CBR */
  /* Baseline profile has no B-frames, but be explicit: reorder latency would
   * otherwise be attributed to the transport. */
  AMediaFormat_setInt32(fmt, "max-bframes", 0);

  media_status_t st =
      AMediaCodec_configure(e->codec, fmt, NULL, NULL,
                            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(fmt);
  if (st != AMEDIA_OK) {
    ELOGE_BOTH("AMediaCodec_configure failed: %d", (int)st);
    AMediaCodec_delete(e->codec);
    free(e);
    return NULL;
  }

  query_codec_name(e->codec, &e->info);

  if (c.require_hardware && e->info.hardware_accelerated == 0) {
    ELOGE_BOTH("REJECTED: '%s' is a software encoder. A software run changes "
               "the encode cost inside every latency number, so it is not "
               "measured rather than footnoted.",
               e->info.codec_name);
    AMediaCodec_delete(e->codec);
    free(e);
    return NULL;
  }

  st = AMediaCodec_start(e->codec);
  if (st != AMEDIA_OK) {
    ELOGE_BOTH("AMediaCodec_start failed: %d", (int)st);
    AMediaCodec_delete(e->codec);
    free(e);
    return NULL;
  }

  e->info.width = c.width;
  e->info.height = c.height;
  e->info.fps = c.fps;
  e->info.bitrate_bps = c.bitrate_bps;
  e->info.keyframe_interval_s = c.keyframe_interval_s;
  e->info.color_format = COLOR_FormatYUV420SemiPlanar;

  ELOG_BOTH("encoder: %s %dx%d@%dfps %d bps keyint=%ds hw=%s",
            e->info.codec_name[0] ? e->info.codec_name : "(name unavailable)",
            c.width, c.height, c.fps, c.bitrate_bps, c.keyframe_interval_s,
            e->info.hardware_accelerated == 1
                ? "yes"
                : (e->info.hardware_accelerated == 0 ? "NO" : "undetermined"));
  if (e->info.hardware_accelerated == -1) {
    ELOG_BOTH("WARNING: hardware acceleration could not be verified on this "
              "API level; record this against the run.");
  }
  return e;
}

void ucv_encoder_destroy(ucv_encoder_t *e) {
  if (!e)
    return;
  if (e->codec) {
    AMediaCodec_stop(e->codec);
    AMediaCodec_delete(e->codec);
  }
  free(e->csd);
  free(e->out_buf);
  free(e);
}

void ucv_encoder_get_info(const ucv_encoder_t *e, ucv_encoder_info_t *out) {
  if (e && out)
    *out = e->info;
}

int ucv_encoder_acquire_input(ucv_encoder_t *e, uint8_t **out_data,
                              size_t *out_capacity, int timeout_us) {
  if (!out_data || !out_capacity)
    return -1;
  *out_data = NULL;
  *out_capacity = 0;
  if (!e || e->input_index >= 0)
    return -1;

  /* Reject a frame before decoding when the codec is saturated. A bounded
   * wait preserves the capture pacing used by the experiment. */
  ssize_t idx = AMediaCodec_dequeueInputBuffer(e->codec, timeout_us);
  if (idx < 0) {
    e->drops++;
    return 0;
  }

  size_t capacity = 0;
  uint8_t *data = AMediaCodec_getInputBuffer(e->codec, (size_t)idx, &capacity);
  e->input_index = idx;
  e->input_capacity = capacity;
  if (!data) {
    ELOGE_BOTH("AMediaCodec_getInputBuffer returned NULL");
    (void)ucv_encoder_discard_input(e);
    e->drops++;
    return -1;
  }

  *out_data = data;
  *out_capacity = capacity;
  return 1;
}

int ucv_encoder_queue_input(ucv_encoder_t *e, size_t size, int64_t pts_us) {
  if (!e || e->input_index < 0)
    return -1;
  if (size > e->input_capacity) {
    ELOGE_BOTH("input buffer too small: cap=%zu need=%zu",
               e->input_capacity, size);
    (void)ucv_encoder_discard_input(e);
    e->drops++;
    return -1;
  }

  const size_t idx = (size_t)e->input_index;
  e->input_index = -1;
  e->input_capacity = 0;
  media_status_t st =
      AMediaCodec_queueInputBuffer(e->codec, idx, 0, size, pts_us, 0);
  if (st != AMEDIA_OK) {
    ELOGE_BOTH("queueInputBuffer failed: %d", (int)st);
    e->drops++;
    return -1;
  }
  e->frames_in++;
  return 0;
}

int ucv_encoder_discard_input(ucv_encoder_t *e) {
  if (!e || e->input_index < 0)
    return -1;

  const size_t idx = (size_t)e->input_index;
  e->input_index = -1;
  e->input_capacity = 0;
  /* MediaCodec exposes no cancel-input call. Queueing an empty, flagless
   * buffer returns ownership without submitting a video frame. */
  media_status_t st = AMediaCodec_queueInputBuffer(e->codec, idx, 0, 0, 0, 0);
  if (st != AMEDIA_OK) {
    ELOGE_BOTH("discard input buffer failed: %d", (int)st);
    return -1;
  }
  return 0;
}

int ucv_encoder_submit_nv12(ucv_encoder_t *e, const uint8_t *nv12, size_t size,
                            int64_t pts_us) {
  if (!e || !nv12)
    return -1;

  uint8_t *buf = NULL;
  size_t cap = 0;
  const int acquired = ucv_encoder_acquire_input(e, &buf, &cap, 5000);
  if (acquired <= 0)
    return acquired < 0 ? -1 : 0;
  if (cap < size) {
    ELOGE_BOTH("input buffer too small: cap=%zu need=%zu", cap, size);
    (void)ucv_encoder_discard_input(e);
    e->drops++;
    return -1;
  }

  memcpy(buf, nv12, size);
  return ucv_encoder_queue_input(e, size, pts_us);
}

static int ensure_out_cap(ucv_encoder_t *e, size_t need) {
  if (e->out_cap >= need)
    return 0;
  uint8_t *p = (uint8_t *)realloc(e->out_buf, need);
  if (!p)
    return -1;
  e->out_buf = p;
  e->out_cap = need;
  return 0;
}

int ucv_encoder_drain(ucv_encoder_t *e, const uint8_t **out_data,
                      size_t *out_size, int64_t *out_pts_us,
                      int *out_is_keyframe, int timeout_us) {
  if (!e)
    return -1;

  for (;;) {
    AMediaCodecBufferInfo info;
    ssize_t idx = AMediaCodec_dequeueOutputBuffer(e->codec, &info, timeout_us);

    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      return 0;
    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
        idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
      continue;
    if (idx < 0)
      return 0;

    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getOutputBuffer(e->codec, (size_t)idx, &cap);
    if (!buf) {
      AMediaCodec_releaseOutputBuffer(e->codec, (size_t)idx, false);
      continue;
    }

    /* SPS/PPS arrives once, before any frame. Stash it and keep draining —
     * surfacing it as a "frame" would corrupt frame_seq and therefore the
     * loss statistics. */
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
      free(e->csd);
      e->csd = (uint8_t *)malloc((size_t)info.size);
      if (e->csd) {
        memcpy(e->csd, buf + info.offset, (size_t)info.size);
        e->csd_size = (size_t)info.size;
        ELOG_BOTH("encoder: captured %zu-byte codec config (SPS/PPS)",
                  e->csd_size);
      }
      AMediaCodec_releaseOutputBuffer(e->codec, (size_t)idx, false);
      continue;
    }

    /* Empty input buffers return a slot after a JPEG decode failure. Most
     * codecs emit nothing for them; ignore a zero-sized output defensively if
     * a vendor codec surfaces one. */
    if (info.size <= 0) {
      AMediaCodec_releaseOutputBuffer(e->codec, (size_t)idx, false);
      continue;
    }

    const int is_key =
        (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) ? 1 : 0;

    /* Repeat SPS/PPS at every random-access point. Sending it only once makes
     * RTSP/WebRTC late join and HLS segments after the first dependent on bytes
     * that are no longer available. Doing it here keeps all transports
     * consistent and lets RTMP discard the in-band copies after building its
     * AVC sequence header. */
    const int prepend = (is_key && e->csd) ? 1 : 0;
    const size_t total =
        (size_t)info.size + (prepend ? e->csd_size : 0);

    if (ensure_out_cap(e, total) != 0) {
      AMediaCodec_releaseOutputBuffer(e->codec, (size_t)idx, false);
      return -1;
    }

    size_t off = 0;
    if (prepend) {
      memcpy(e->out_buf, e->csd, e->csd_size);
      off = e->csd_size;
    }
    memcpy(e->out_buf + off, buf + info.offset, (size_t)info.size);

    /* Released immediately: holding codec buffers stalls the encoder and
     * would show up as encode latency that is really our own bookkeeping. */
    AMediaCodec_releaseOutputBuffer(e->codec, (size_t)idx, false);

    e->frames_out++;
    *out_data = e->out_buf;
    *out_size = total;
    *out_pts_us = info.presentationTimeUs;
    *out_is_keyframe = is_key;
    return 1;
  }
}

uint64_t ucv_encoder_frames_in(const ucv_encoder_t *e)  { return e ? e->frames_in : 0; }
uint64_t ucv_encoder_frames_out(const ucv_encoder_t *e) { return e ? e->frames_out : 0; }
uint64_t ucv_encoder_drops(const ucv_encoder_t *e)      { return e ? e->drops : 0; }

#else

/* Host smoke tests compile this file with UCV_JPEG_ONLY. Keep the decoder
 * implementation identical to Android while replacing only its log sink. */
#define ELOG_BOTH(...)             \
  do {                             \
    fprintf(stdout, __VA_ARGS__);  \
    fputc('\n', stdout);           \
  } while (0)
#define ELOGE_BOTH(...)            \
  do {                             \
    fprintf(stderr, __VA_ARGS__);  \
    fputc('\n', stderr);           \
  } while (0)

#endif

/* ================================================================ */
/* MJPEG -> NV12                                                     */
/* ================================================================ */

/* libjpeg's default error handler calls exit() — unacceptable inside a
 * streaming server, where one corrupt frame must not take the process down.
 * longjmp back to the caller instead. */
struct jpeg_err_ctx {
  struct jpeg_error_mgr pub;
  jmp_buf               jump;
};

#define UCV_JPEG_COMPONENTS 3
#define UCV_JPEG_RAW_ROWS   (MAX_SAMP_FACTOR * DCTSIZE)

struct ucv_jpeg_decoder {
  int      width, height;
  uint8_t *nv12;      /* Y plane, then interleaved UV (NV12 order) */
  size_t   nv12_size;
  size_t   raw_stride;
  uint8_t *raw[UCV_JPEG_COMPONENTS];
  JSAMPROW raw_rows[UCV_JPEG_COMPONENTS][UCV_JPEG_RAW_ROWS];

  /* Creating and destroying libjpeg's permanent pools for every camera frame
   * is pure allocator overhead. This object is owned by the single capture
   * thread, so one decompressor can safely serve the whole pipeline run. */
  struct jpeg_decompress_struct cinfo;
  struct jpeg_err_ctx           err;
  int                           jpeg_created;
  int                           layout_logged;
};

static void jpeg_error_exit_longjmp(j_common_ptr cinfo) {
  struct jpeg_err_ctx *ctx = (struct jpeg_err_ctx *)cinfo->err;
  longjmp(ctx->jump, 1);
}

static void jpeg_output_message_silent(j_common_ptr cinfo) {
  (void)cinfo; /* corrupt frames are counted by the caller, not logged per-frame */
}

ucv_jpeg_decoder_t *ucv_jpeg_decoder_create(int width, int height) {
  if (width <= 0 || height <= 0)
    return NULL;
  /* NV12 needs even dimensions for the subsampled chroma plane. */
  if ((width & 1) || (height & 1)) {
    ELOGE_BOTH("jpeg decoder: %dx%d has odd dimensions, NV12 requires even",
               width, height);
    return NULL;
  }

  ucv_jpeg_decoder_t *d = (ucv_jpeg_decoder_t *)calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->width = width;
  d->height = height;
  d->nv12_size = (size_t)width * height * 3 / 2;
  d->raw_stride = ((size_t)width + DCTSIZE - 1) / DCTSIZE * DCTSIZE;
  const size_t raw_bytes = d->raw_stride * UCV_JPEG_RAW_ROWS;
  for (int component = 0; component < UCV_JPEG_COMPONENTS; component++) {
    d->raw[component] = (uint8_t *)malloc(raw_bytes);
    if (!d->raw[component]) {
      ucv_jpeg_decoder_destroy(d);
      return NULL;
    }
    for (int row = 0; row < UCV_JPEG_RAW_ROWS; row++)
      d->raw_rows[component][row] =
          d->raw[component] + (size_t)row * d->raw_stride;
  }
  d->cinfo.err = jpeg_std_error(&d->err.pub);
  d->err.pub.error_exit = jpeg_error_exit_longjmp;
  d->err.pub.output_message = jpeg_output_message_silent;
  if (setjmp(d->err.jump)) {
    ELOGE_BOTH("jpeg decoder: libjpeg initialization failed");
    ucv_jpeg_decoder_destroy(d);
    return NULL;
  }
  d->jpeg_created = 1;
  /* libjpeg documents jpeg_destroy() as safe even when create fails, so mark
   * it before the call and let the longjmp cleanup path release partial state. */
  jpeg_create_decompress(&d->cinfo);
  return d;
}

void ucv_jpeg_decoder_destroy(ucv_jpeg_decoder_t *d) {
  if (!d)
    return;
  if (d->jpeg_created)
    jpeg_destroy_decompress(&d->cinfo);
  free(d->nv12);
  for (int component = 0; component < UCV_JPEG_COMPONENTS; component++)
    free(d->raw[component]);
  free(d);
}

static int jpeg_raw_layout(const struct jpeg_decompress_struct *cinfo,
                           int *h_expand, int *v_expand) {
  if (cinfo->jpeg_color_space == JCS_GRAYSCALE && cinfo->num_components == 1) {
    *h_expand = *v_expand = 0;
    return 1;
  }
  if (cinfo->jpeg_color_space != JCS_YCbCr || cinfo->num_components != 3)
    return 0;

  const jpeg_component_info *y = &cinfo->comp_info[0];
  const jpeg_component_info *cb = &cinfo->comp_info[1];
  const jpeg_component_info *cr = &cinfo->comp_info[2];
  if (y->h_samp_factor != cinfo->max_h_samp_factor ||
      y->v_samp_factor != cinfo->max_v_samp_factor ||
      cb->h_samp_factor != cr->h_samp_factor ||
      cb->v_samp_factor != cr->v_samp_factor ||
      cb->h_samp_factor <= 0 || cb->v_samp_factor <= 0 ||
      cinfo->max_h_samp_factor % cb->h_samp_factor != 0 ||
      cinfo->max_v_samp_factor % cb->v_samp_factor != 0)
    return 0;

  *h_expand = cinfo->max_h_samp_factor / cb->h_samp_factor;
  *v_expand = cinfo->max_v_samp_factor / cb->v_samp_factor;
  return 1;
}

int ucv_jpeg_decode_into_nv12(ucv_jpeg_decoder_t *d, const uint8_t *jpeg,
                              size_t jpeg_size, uint8_t *nv12,
                              size_t nv12_capacity, size_t *out_size) {
  if (!d || !jpeg || !jpeg_size || !nv12 || !out_size ||
      nv12_capacity < d->nv12_size)
    return -1;

  struct jpeg_decompress_struct *cinfo = &d->cinfo;
  if (setjmp(d->err.jump)) {
    /* jpeg_abort_decompress() is one of the two libjpeg calls documented as
     * safe after error_exit. It releases per-image pools but preserves the
     * reusable decompressor and its permanent allocations. */
    jpeg_abort_decompress(cinfo);
    return -1; /* corrupt frame: caller counts it, stream keeps running */
  }

  jpeg_mem_src(cinfo, jpeg, (unsigned long)jpeg_size);
  if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_abort_decompress(cinfo);
    return -1;
  }

  int h_expand = 0, v_expand = 0;
  if (!jpeg_raw_layout(cinfo, &h_expand, &v_expand)) {
    ELOGE_BOTH("jpeg decoder: unsupported color space or sampling layout");
    jpeg_abort_decompress(cinfo);
    return -1;
  }

  /* Bypass libjpeg's chroma upsampler and interleaved YCbCr scanline output.
   * The camera's native component planes are copied/resampled directly into
   * NV12, eliminating the former 3-bytes-per-pixel scratch path. */
  cinfo->raw_data_out = TRUE;
  jpeg_start_decompress(cinfo);

  if ((int)cinfo->output_width != d->width ||
      (int)cinfo->output_height != d->height) {
    ELOGE_BOTH("jpeg decoder: frame is %dx%d but decoder configured %dx%d",
               (int)cinfo->output_width, (int)cinfo->output_height, d->width,
               d->height);
    jpeg_abort_decompress(cinfo);
    return -1;
  }
  if (!d->layout_logged) {
    if (cinfo->jpeg_color_space == JCS_GRAYSCALE)
      ELOG_BOTH("jpeg decoder: raw grayscale -> NV12");
    else
      ELOG_BOTH("jpeg decoder: raw YCbCr -> NV12 (chroma scale %dx%d)",
                 h_expand, v_expand);
    d->layout_logged = 1;
  }

  const int w = d->width, h = d->height;
  uint8_t *Y = nv12;
  uint8_t *UV = nv12 + (size_t)w * h;
  const JDIMENSION lines_per_iMCU =
      (JDIMENSION)cinfo->max_v_samp_factor * DCTSIZE;
  JSAMPARRAY planes[UCV_JPEG_COMPONENTS] = {
      d->raw_rows[0], d->raw_rows[1], d->raw_rows[2]};

  for (int component = 0; component < cinfo->num_components; component++) {
    if ((size_t)cinfo->comp_info[component].width_in_blocks * DCTSIZE >
        d->raw_stride) {
      ELOGE_BOTH("jpeg decoder: raw component exceeds scratch stride");
      jpeg_abort_decompress(cinfo);
      return -1;
    }
  }

  while ((int)cinfo->output_scanline < h) {
    const int y_base = (int)cinfo->output_scanline;
    if (jpeg_read_raw_data(cinfo, planes, lines_per_iMCU) == 0) {
      jpeg_abort_decompress(cinfo);
      return -1;
    }
    const int rows = h - y_base < (int)lines_per_iMCU
        ? h - y_base : (int)lines_per_iMCU;

    for (int row = 0; row < rows; row++)
      memcpy(Y + (size_t)(y_base + row) * w, d->raw_rows[0][row], (size_t)w);

    for (int row = 0; row < rows; row++) {
      const int output_y = y_base + row;
      if (output_y & 1)
        continue;
      uint8_t *uv = UV + (size_t)(output_y / 2) * w;
      if (cinfo->jpeg_color_space == JCS_GRAYSCALE) {
        memset(uv, 128, (size_t)w);
        continue;
      }

      const uint8_t *cb = d->raw_rows[1][row / v_expand];
      const uint8_t *cr = d->raw_rows[2][row / v_expand];
      for (int x = 0; x < w; x += 2) {
        const int source_x = x / h_expand;
        uv[x + 0] = cb[source_x];
        uv[x + 1] = cr[source_x];
      }
    }
  }

  jpeg_finish_decompress(cinfo);

  *out_size = d->nv12_size;
  return 0;
}

int ucv_jpeg_decode_to_nv12(ucv_jpeg_decoder_t *d, const uint8_t *jpeg,
                            size_t jpeg_size, const uint8_t **out_nv12,
                            size_t *out_size) {
  if (!d || !out_nv12 || !out_size)
    return -1;
  if (!d->nv12) {
    d->nv12 = (uint8_t *)malloc(d->nv12_size);
    if (!d->nv12)
      return -1;
  }
  if (ucv_jpeg_decode_into_nv12(d, jpeg, jpeg_size, d->nv12,
                                d->nv12_size, out_size) != 0)
    return -1;
  *out_nv12 = d->nv12;
  return 0;
}
