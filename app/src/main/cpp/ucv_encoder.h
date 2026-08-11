/* H.264 encoder for the protocol experiment.
 *
 * Wraps MediaCodec via the NDK (AMediaCodec) so capture -> decode -> encode
 * -> transport all run in native code on one clock. A Kotlin/JNI encoder
 * would add a per-frame array copy and put the encode timestamp on a
 * different code path from capture, which is exactly the kind of measurement
 * contamination the harness spec exists to prevent.
 *
 * The configuration here is the FROZEN one from
 * experiment/docs/01-harness-spec.md section 2. It is not per-protocol and
 * must not be varied between runs — if it changes, every previously measured
 * run is invalidated.
 */

#ifndef UCV_ENCODER_H
#define UCV_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frozen encoder configuration (harness spec section 2). Defaults are the
 * agreed values; the struct exists so a run can REPORT what it actually used
 * rather than assuming, and so a deliberate deviation is explicit. */
typedef struct {
  int32_t width;
  int32_t height;
  int32_t fps;
  int32_t bitrate_bps;
  int32_t keyframe_interval_s;
  int32_t require_hardware; /* 1 = fail rather than run on a software codec */
} ucv_encoder_config_t;

#define UCV_ENC_DEFAULT_WIDTH       1280
#define UCV_ENC_DEFAULT_HEIGHT      720
#define UCV_ENC_DEFAULT_FPS         30
#define UCV_ENC_DEFAULT_BITRATE     4000000
#define UCV_ENC_DEFAULT_KEYINT_S    1

/* What the encoder actually negotiated. Reported to the receiver so a run
 * whose config does not match the frozen values can be marked INVALID
 * instead of quietly polluting the comparison. */
typedef struct {
  char    codec_name[128];
  int32_t hardware_accelerated;  /* -1 = could not be determined */
  int32_t width, height, fps, bitrate_bps, keyframe_interval_s;
  int32_t color_format;
} ucv_encoder_info_t;

typedef struct ucv_encoder ucv_encoder_t;

/* Creates and starts the encoder. Returns NULL on failure (reason is logged).
 * If cfg->require_hardware is set and a hardware encoder cannot be confirmed,
 * this FAILS rather than falling back — a software encoder silently changes
 * the encode cost baked into every latency number. */
ucv_encoder_t *ucv_encoder_create(const ucv_encoder_config_t *cfg);

void ucv_encoder_destroy(ucv_encoder_t *enc);

void ucv_encoder_get_info(const ucv_encoder_t *enc, ucv_encoder_info_t *out);

/* Submits one NV12 frame. `pts_us` must be monotonic; MediaCodec uses it for
 * rate control, and a non-monotonic value quietly degrades output quality.
 * Returns 0 on success, negative on error. */
int ucv_encoder_submit_nv12(ucv_encoder_t *enc, const uint8_t *nv12,
                            size_t size, int64_t pts_us);

/* Drains one encoded access unit if available.
 *
 * Returns  1  a frame was written to *out_data / *out_size
 *          0  nothing ready yet (not an error)
 *         <0  error
 *
 * The returned pointer is owned by the encoder and stays valid only until the
 * next call to this function — callers must send or copy it before draining
 * again. Codec config (SPS/PPS) is captured internally and prepended to the
 * first keyframe rather than surfaced as a separate frame, so the transport
 * layer never has to special-case it. */
int ucv_encoder_drain(ucv_encoder_t *enc, const uint8_t **out_data,
                      size_t *out_size, int64_t *out_pts_us,
                      int *out_is_keyframe, int timeout_us);

/* Counters for the run summary. */
uint64_t ucv_encoder_frames_in(const ucv_encoder_t *enc);
uint64_t ucv_encoder_frames_out(const ucv_encoder_t *enc);
uint64_t ucv_encoder_drops(const ucv_encoder_t *enc);

/* ---------------------------------------------------------------- */
/* MJPEG -> NV12 conversion                                          */
/* ---------------------------------------------------------------- */
/* libjpeg-turbo is already vendored and NEON-enabled in this build, so the
 * decode is a wiring job rather than an integration. Decodes straight to
 * NV12 (MediaCodec's expected input) instead of via RGB, which avoids a
 * full-frame colour conversion per frame. */

typedef struct ucv_jpeg_decoder ucv_jpeg_decoder_t;

ucv_jpeg_decoder_t *ucv_jpeg_decoder_create(int width, int height);
void                ucv_jpeg_decoder_destroy(ucv_jpeg_decoder_t *d);

/* Decodes a JPEG into the decoder's internal NV12 buffer. The decoder also
 * retains its libjpeg state across calls, so create one per pipeline run rather
 * than one per frame. Returns 0 on success; *out_nv12 / *out_size point at
 * internal storage valid until the next decode call. */
int ucv_jpeg_decode_to_nv12(ucv_jpeg_decoder_t *d, const uint8_t *jpeg,
                            size_t jpeg_size, const uint8_t **out_nv12,
                            size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* UCV_ENCODER_H */
