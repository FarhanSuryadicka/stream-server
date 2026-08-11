/* Experiment pipeline: UVC capture -> JPEG decode -> H.264 encode -> transport.
 *
 * This is the shared path ALL seven protocols run through. Only the transport
 * at the end differs; capture, decode, encode and instrumentation are common
 * code, which is what makes the protocol comparison valid at all (harness
 * spec section 1).
 */

#ifndef UCV_PIPELINE_H
#define UCV_PIPELINE_H

#include <stdint.h>
#include "ucv_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint64_t frames_captured;
  uint64_t frames_decoded;
  uint64_t frames_encoded;
  uint64_t frames_sent;
  uint64_t decode_errors;
  uint64_t encoder_drops;
  uint64_t send_errors;
  uint64_t bytes_payload;
  uint64_t bytes_wire;
  /* Rolling averages in microseconds, for the on-screen log. The
   * authoritative per-frame numbers live in the wire timestamps; these exist
   * so the operator can see the rig is healthy without a PC attached. */
  uint32_t avg_decode_us;
  uint32_t avg_encode_us;
} ucv_pipeline_stats_t;

/* Starts the pipeline. `strmh` is an already-started uvc_stream_handle_t
 * (void* to keep libuvc out of this header). `peer_cfg` is transport
 * specific — for Raw UDP, "ip:port".
 *
 * Returns 0 on success. Fails rather than falling back if the requested
 * protocol has no implementation, so a run can never be mislabelled. */
int  ucv_pipeline_start(void *strmh, ucv_protocol_t proto, const char *peer_cfg,
                        int width, int height, int fps);
void ucv_pipeline_stop(void);
int  ucv_pipeline_running(void);

void ucv_pipeline_get_stats(ucv_pipeline_stats_t *out);

/* Encoder config actually negotiated, formatted for the on-screen log and
 * for recording against the run (harness spec section 6.1). */
const char *ucv_pipeline_encoder_desc(void);

#ifdef __cplusplus
}
#endif

#endif /* UCV_PIPELINE_H */
