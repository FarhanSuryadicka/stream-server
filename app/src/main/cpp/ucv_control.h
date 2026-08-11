/* Upstream control channel — see ucv_control.c for rationale. */

#ifndef UCV_CONTROL_H
#define UCV_CONTROL_H

#include <stdint.h>
#include "ucv_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fisheye view parameters driven from the PC. */
typedef struct {
  float   alpha;
  float   beta;
  float   zoom;
  uint8_t preset;
} ucv_view_state_t;

typedef struct {
  uint64_t cmds_received;
  uint64_t cmds_dropped_crc;
  uint64_t reorder_events;
} ucv_control_stats_t;

typedef int (*ucv_remote_start_fn)(const char *peer_ip,
                                   const ucv_start_run_payload_t *request);
typedef void (*ucv_remote_stop_fn)(void);
typedef int (*ucv_remote_mode_fn)(int index, uint16_t *width,
                                  uint16_t *height, uint8_t *fps);

/* Starts the UDP control listener. Pass 0 for the default port. */
int  ucv_control_start(int port);
void ucv_control_stop(void);

void ucv_control_get_view(ucv_view_state_t *out);
void ucv_control_get_stats(ucv_control_stats_t *out);
void ucv_control_set_callbacks(ucv_remote_start_fn start_fn,
                               ucv_remote_stop_fn stop_fn,
                               ucv_remote_mode_fn mode_fn);

#ifdef __cplusplus
}
#endif

#endif /* UCV_CONTROL_H */
