/* Transport abstraction for the protocol experiment.
 *
 * The whole point of the experiment is that the ONLY thing that differs
 * between the seven protocols is the transport. Capture, decode, encode and
 * instrumentation are shared code above this interface, so a protocol cannot
 * accidentally be measured with a different encoder configuration — the
 * failure mode the harness spec exists to prevent.
 *
 * Every transport receives an already-encoded H.264 access unit plus the
 * timestamps collected upstream, and is responsible only for getting those
 * bytes to the PC.
 */

#ifndef UCV_TRANSPORT_H
#define UCV_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "ucv_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One encoded frame handed to a transport. Timestamps are already populated
 * by the shared pipeline; a transport must not re-stamp them (except t_sent,
 * which it stamps as late as possible — see ucv_transport_send). */
typedef struct {
  const uint8_t *data;
  size_t         size;
  uint32_t       seq;
  uint64_t       t_capture_ns;
  uint64_t       t_encoded_ns;
  int            is_keyframe;
} ucv_encoded_frame_t;

typedef struct ucv_transport ucv_transport_t;

struct ucv_transport {
  const char *name;
  uint16_t    protocol_id;

  /* Bring the transport up. `cfg` is transport-specific (peer address,
   * port, passphrase...). Returns 0 on success, negative errno-style on
   * failure. Must be safe to call after a failed start. */
  int (*start)(ucv_transport_t *self, const char *cfg);

  /* Send one encoded frame. Implementations MUST stamp t_sent_ns
   * immediately before the actual write syscall — stamping earlier folds
   * application queueing into the measured transport latency and flatters
   * slow transports. Returns 0 on success. */
  int (*send)(ucv_transport_t *self, const ucv_encoded_frame_t *frame);

  /* Tear down. Must be idempotent. */
  void (*stop)(ucv_transport_t *self);

  /* Transport-private state. */
  void *impl;

  /* Counters, read by the summary line of the sender log. Maintained by the
   * transport because only it knows what a "wire byte" means for its own
   * framing. */
  uint64_t frames_sent;
  uint64_t bytes_payload;
  uint64_t bytes_wire;
  uint64_t send_errors;
};

/* ---------------------------------------------------------------- */
/* Registry                                                          */
/* ---------------------------------------------------------------- */
/* Returns NULL for a protocol that is not yet implemented — the caller
 * reports that honestly rather than silently falling back to another
 * transport, which would produce a mislabelled run. */
ucv_transport_t *ucv_transport_get(ucv_protocol_t proto);

/* Individual constructors — exposed for testing. */
ucv_transport_t *ucv_transport_rawudp(void);
ucv_transport_t *ucv_transport_mjpeg(void);
ucv_transport_t *ucv_transport_rtp(void);
ucv_transport_t *ucv_transport_srt(void);
ucv_transport_t *ucv_transport_rtsp(void);
ucv_transport_t *ucv_transport_webrtc(void);
ucv_transport_t *ucv_transport_rtmps(void);
ucv_transport_t *ucv_transport_hls(void);

/* ---------------------------------------------------------------- */
/* Shared helpers                                                    */
/* ---------------------------------------------------------------- */

/* Monotonic nanoseconds — the timebase for every phone-side timestamp.
 * CLOCK_MONOTONIC, not REALTIME: an NTP step mid-run would otherwise produce
 * negative latencies. */
uint64_t ucv_now_ns(void);

/* run_id hash for the current run, set by the pipeline at run start. */
void     ucv_set_run_id(const char *run_id);
void     ucv_set_run_id_hash(uint32_t run_id_hash);
uint32_t ucv_get_run_id_hash(void);

/* Fills the shared preamble fields. Transports call this, then stamp
 * t_sent_ns and finalize, so every protocol reports identical metadata. */
void ucv_fill_header(ucv_frame_header_t *h, const ucv_encoded_frame_t *f,
                     uint16_t protocol_id, uint32_t payload_bytes,
                     uint8_t extra_flags);

#ifdef __cplusplus
}
#endif

#endif /* UCV_TRANSPORT_H */
