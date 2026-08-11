/* Upstream control channel (PC -> phone) + clock-sync responder.
 *
 * The issue is explicit that a protocol which cannot carry or pair with a
 * low-latency back-channel is not a winner, so the upstream path is measured
 * as a first-class result rather than assumed to be free.
 *
 * This is a dedicated UDP socket used by EVERY protocol, including those with
 * a native back-channel. Holding the control mechanism constant means the
 * control-RTT column compares like with like; where a native channel exists
 * (WebRTC data channel), it is measured additionally so "paired" and "native"
 * can be compared honestly rather than conflated.
 *
 * It also answers PING, which is what makes clock synchronisation possible —
 * without it every one-way latency number in the experiment would be fiction
 * (harness spec §4).
 */

#include "ucv_control.h"
#include "ucv_wire.h"
#include "ucv_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <android/log.h>

#define TAG "ucv-control"
#define CLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define CLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static int          g_sock = -1;
static pthread_t    g_thread;
static volatile int g_quit = 1;
static int          g_running = 0;

/* Current view parameters, updated by inbound commands. The dewarp pipeline
 * reads these; for the transport experiment they mostly exist so the command
 * path is realistic rather than a no-op that would understate apply cost. */
static ucv_view_state_t g_view = {.alpha = 0.0f, .beta = 0.0f, .zoom = 1.0f, .preset = 0};
static pthread_mutex_t  g_view_lock = PTHREAD_MUTEX_INITIALIZER;

/* Counters for the run summary. */
static uint64_t g_cmds_received = 0;
static uint64_t g_cmds_dropped_crc = 0;
static uint32_t g_last_cmd_seq = 0;
static uint64_t g_reorder_events = 0;
static ucv_remote_start_fn g_start_fn = NULL;
static ucv_remote_stop_fn  g_stop_fn = NULL;
static ucv_remote_mode_fn  g_mode_fn = NULL;

void ucv_control_set_callbacks(ucv_remote_start_fn start_fn,
                               ucv_remote_stop_fn stop_fn,
                               ucv_remote_mode_fn mode_fn) {
  g_start_fn = start_fn;
  g_stop_fn = stop_fn;
  g_mode_fn = mode_fn;
}

void ucv_control_get_view(ucv_view_state_t *out) {
  pthread_mutex_lock(&g_view_lock);
  *out = g_view;
  pthread_mutex_unlock(&g_view_lock);
}

void ucv_control_get_stats(ucv_control_stats_t *out) {
  out->cmds_received    = g_cmds_received;
  out->cmds_dropped_crc = g_cmds_dropped_crc;
  out->reorder_events   = g_reorder_events;
}

/* Reads a float32 out of the 8-byte payload without punning through a
 * pointer cast — the payload is not guaranteed to be 4-byte aligned inside
 * the packed struct, and an unaligned load is UB even where arm64 tolerates
 * it in practice. */
static float payload_f32(const uint8_t *p) {
  float v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static void *control_thread(void *arg) {
  (void)arg;
  CLOGI("control thread started");

  while (!g_quit) {
    ucv_control_t      cmd;
    struct sockaddr_in from;
    socklen_t          fromlen = sizeof(from);

    ssize_t n = recvfrom(g_sock, &cmd, sizeof(cmd), 0,
                         (struct sockaddr *)&from, &fromlen);
    /* Stamped immediately on return from recvfrom: this is t2 in the
     * clock-sync exchange, and any work done before stamping would be
     * charged to the network path. */
    uint64_t t_recv = ucv_now_ns();

    if (n < 0) {
      if (g_quit)
        break;
      continue;
    }
    if (n != (ssize_t)sizeof(ucv_control_t))
      continue; /* not one of ours */

    if (!ucv_control_valid(&cmd)) {
      g_cmds_dropped_crc++;
      continue;
    }

    g_cmds_received++;

    /* Ordering matters for a control surface: an out-of-order SET_ALPHA
     * leaves the view stuck at a stale angle, so reordering is counted
     * rather than silently tolerated. Wrap-safe signed comparison. */
    if (g_last_cmd_seq && (int32_t)(cmd.cmd_seq - g_last_cmd_seq) < 0)
      g_reorder_events++;
    else
      g_last_cmd_seq = cmd.cmd_seq;

    int applied = 1;
    uint64_t reply_data1 = 0, reply_data2 = 0;
    switch (cmd.cmd_type) {
      case UCV_CMD_SET_ALPHA:
        pthread_mutex_lock(&g_view_lock);
        g_view.alpha = payload_f32(cmd.payload);
        pthread_mutex_unlock(&g_view_lock);
        break;
      case UCV_CMD_SET_BETA:
        pthread_mutex_lock(&g_view_lock);
        g_view.beta = payload_f32(cmd.payload);
        pthread_mutex_unlock(&g_view_lock);
        break;
      case UCV_CMD_SET_ZOOM:
        pthread_mutex_lock(&g_view_lock);
        g_view.zoom = payload_f32(cmd.payload);
        pthread_mutex_unlock(&g_view_lock);
        break;
      case UCV_CMD_SET_PRESET:
        pthread_mutex_lock(&g_view_lock);
        g_view.preset = cmd.payload[0];
        pthread_mutex_unlock(&g_view_lock);
        break;
      case UCV_CMD_PING:
        /* Nothing to apply — the ACK timestamps are the payload. */
        break;
      case UCV_CMD_START_RUN:
        if (g_start_fn) {
          ucv_start_run_payload_t request;
          char peer_ip[INET_ADDRSTRLEN] = {0};
          memcpy(&request, cmd.payload, sizeof(request));
          if (!inet_ntop(AF_INET, &from.sin_addr, peer_ip, sizeof(peer_ip)) ||
              g_start_fn(peer_ip, &request) != 0)
            applied = 0;
        } else {
          applied = 0;
        }
        break;
      case UCV_CMD_STOP_RUN:
        if (g_stop_fn)
          g_stop_fn();
        else
          applied = 0;
        break;
      case UCV_CMD_SET_RUN_HASH: {
        uint32_t hash = 0;
        memcpy(&hash, cmd.payload, sizeof(hash));
        ucv_set_run_id_hash(hash);
        break;
      }
      case UCV_CMD_GET_MODE: {
        uint16_t index = 0, width = 0, height = 0;
        uint8_t fps = 0;
        memcpy(&index, cmd.payload, sizeof(index));
        if (!g_mode_fn || g_mode_fn(index, &width, &height, &fps) != 0) {
          applied = 0;
        } else {
          /* GET_MODE does not use clock timestamps. Reuse the two 64-bit
           * response fields without changing the fixed ACK wire size. */
          reply_data1 = ((uint64_t)width << 32) | height;
          reply_data2 = ((uint64_t)fps << 32) | index;
        }
        break;
      }
      default:
        applied = 0;
        break;
    }

    if (cmd.flags & UCV_CTL_ACK_REQUESTED) {
      ucv_ack_t ack;
      memset(&ack, 0, sizeof(ack));
      ack.cmd_type      = cmd.cmd_type;
      ack.flags         = applied ? UCV_ACK_APPLIED : 0;
      ack.cmd_seq       = cmd.cmd_seq;
      ack.t_cmd_sent_ns = cmd.t_sent_ns;
      ack.t_recv_ns     = t_recv;
      ack.t_ack_ns      = ucv_now_ns(); /* t3, stamped last */
      if (cmd.cmd_type == UCV_CMD_GET_MODE && applied) {
        ack.t_recv_ns = reply_data1;
        ack.t_ack_ns = reply_data2;
      }
      ucv_ack_finalize(&ack);

      sendto(g_sock, &ack, sizeof(ack), 0, (struct sockaddr *)&from, fromlen);
    }
  }

  CLOGI("control thread exiting");
  return NULL;
}

int ucv_control_start(int port) {
  if (g_running)
    return 0;
  if (port <= 0)
    port = UCV_PORT_CONTROL;

  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock < 0) {
    CLOGE("control: socket() failed errno=%d", errno);
    return -errno;
  }
  int one = 1;
  setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons((uint16_t)port);
  if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    CLOGE("control: bind failed on %d errno=%d", port, errno);
    close(g_sock);
    g_sock = -1;
    return -errno;
  }

  g_cmds_received = g_cmds_dropped_crc = g_reorder_events = 0;
  g_last_cmd_seq = 0;
  g_quit = 0;
  if (pthread_create(&g_thread, NULL, control_thread, NULL) != 0) {
    close(g_sock);
    g_sock = -1;
    return -EAGAIN;
  }
  g_running = 1;
  CLOGI("control: listening on :%d", port);
  return 0;
}

void ucv_control_stop(void) {
  if (!g_running)
    return;
  g_quit = 1;
  /* shutdown() unblocks the recvfrom; close() alone is not guaranteed to. */
  if (g_sock >= 0) {
    shutdown(g_sock, SHUT_RDWR);
    close(g_sock);
    g_sock = -1;
  }
  pthread_join(g_thread, NULL);
  g_running = 0;
  CLOGI("control: stopped (rx=%llu crc_drop=%llu reorder=%llu)",
        (unsigned long long)g_cmds_received,
        (unsigned long long)g_cmds_dropped_crc,
        (unsigned long long)g_reorder_events);
}
