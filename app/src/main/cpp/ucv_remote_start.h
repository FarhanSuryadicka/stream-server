#ifndef UCV_REMOTE_START_H
#define UCV_REMOTE_START_H

#include "ucv_wire.h"

/* Pure protocol routing helpers shared with the PC self-test. Keeping this
 * table outside jni_bridge.c makes a newly added dashboard protocol fail a
 * test instead of being silently rejected by the phone's START callback. */
static inline uint16_t ucv_remote_default_port(uint16_t protocol_id) {
  switch (protocol_id) {
    case UCV_PROTO_RAWUDP:         return UCV_PORT_RAWUDP;
    case UCV_PROTO_SRT:            return UCV_PORT_SRT;
    case UCV_PROTO_MJPEG:          return UCV_PORT_MJPEG;
    case UCV_PROTO_RTSP:           return UCV_PORT_RTP;
    case UCV_PROTO_WEBRTC:         return UCV_PORT_SIGNAL;
    case UCV_PROTO_RTMPS:          return UCV_PORT_RTMPS;
    case UCV_PROTO_HLS:            return UCV_PORT_HLS;
    case UCV_PROTO_RTSP_SIGNALLED: return UCV_PORT_RTSP;
    default:                       return 0;
  }
}

static inline int ucv_remote_start_supported(uint16_t protocol_id) {
  switch (protocol_id) {
    case UCV_PROTO_RAWUDP:
    case UCV_PROTO_SRT:
    case UCV_PROTO_MJPEG:
    case UCV_PROTO_RTSP:
    case UCV_PROTO_WEBRTC:
    case UCV_PROTO_RTMPS:
    case UCV_PROTO_HLS:
      return 1;
    /* The Android RTSP server exists, but the PC receiver does not yet perform
     * an RTSP session. Reject it rather than producing a mislabeled run. */
    case UCV_PROTO_RTSP_SIGNALLED:
    default:
      return 0;
  }
}

static inline int ucv_remote_config_is_port_only(uint16_t protocol_id) {
  return protocol_id == UCV_PROTO_MJPEG;
}

#endif /* UCV_REMOTE_START_H */
