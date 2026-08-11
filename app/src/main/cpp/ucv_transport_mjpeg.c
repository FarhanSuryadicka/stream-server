/* MJPEG-over-HTTP transport (protocol #5) — instrumented.
 *
 * This is the one protocol the app already spoke, so it becomes the baseline
 * the others are compared against. The difference from the original server in
 * jni_bridge.c is that every multipart part now carries the instrumentation
 * headers from wire-format spec §1.2, without which the stream produces no
 * measurements at all.
 *
 * Kept text/multipart rather than switching to the binary preamble so the
 * stream stays viewable in a plain browser — the property that makes MJPEG
 * worth having in the comparison. The ~130 bytes/frame of header text is
 * counted in bytes_wire and reported as overhead, not excused.
 */

#include "ucv_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <android/log.h>

#define TAG "ucv-mjpeg"
#define MLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define MLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MJPEG_MAX_CLIENTS 4

typedef struct {
  int      lsock;
  int      clients[MJPEG_MAX_CLIENTS];
  pthread_mutex_t lock;
  pthread_t accept_th;
  volatile int quit;
  int      started;
  int      port;
} mjpeg_impl_t;

static int send_all_fd(int s, const uint8_t *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(s, buf + off, len - off, MSG_NOSIGNAL);
    if (n <= 0)
      return -1;
    off += (size_t)n;
  }
  return 0;
}

static void *mjpeg_accept_thread(void *arg) {
  mjpeg_impl_t *im = (mjpeg_impl_t *)arg;
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache, no-store\r\n"
      "Connection: close\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n";

  while (!im->quit) {
    int cs = accept(im->lsock, NULL, NULL);
    if (cs < 0) {
      if (im->quit)
        break;
      continue;
    }

    /* Nagle would coalesce small parts and add tens of milliseconds of
     * delay that belong to the socket option, not to the protocol. Leaving
     * it on would make MJPEG look worse than it is. */
    int one = 1;
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tmo = {5, 0};
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));

    if (send_all_fd(cs, (const uint8_t *)hdr, sizeof(hdr) - 1) < 0) {
      close(cs);
      continue;
    }

    int slot = -1;
    pthread_mutex_lock(&im->lock);
    for (int i = 0; i < MJPEG_MAX_CLIENTS; i++) {
      if (im->clients[i] < 0) {
        im->clients[i] = cs;
        slot = i;
        break;
      }
    }
    pthread_mutex_unlock(&im->lock);

    if (slot < 0) {
      MLOGE("mjpeg: client limit reached, rejecting");
      close(cs);
    } else {
      MLOGI("mjpeg: client connected (slot %d)", slot);
    }
  }
  return NULL;
}

static int mjpeg_start(ucv_transport_t *self, const char *cfg) {
  mjpeg_impl_t *im = (mjpeg_impl_t *)self->impl;
  if (im->started)
    return 0;

  im->port = (cfg && *cfg) ? atoi(cfg) : UCV_PORT_MJPEG;
  if (im->port <= 0)
    im->port = UCV_PORT_MJPEG;

  for (int i = 0; i < MJPEG_MAX_CLIENTS; i++)
    im->clients[i] = -1;

  im->lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (im->lsock < 0) {
    MLOGE("mjpeg: socket() failed errno=%d", errno);
    return -errno;
  }
  int one = 1;
  setsockopt(im->lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons((uint16_t)im->port);
  if (bind(im->lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    MLOGE("mjpeg: bind failed on %d errno=%d", im->port, errno);
    close(im->lsock);
    im->lsock = -1;
    return -errno;
  }
  listen(im->lsock, 8);

  im->quit = 0;
  if (pthread_create(&im->accept_th, NULL, mjpeg_accept_thread, im) != 0) {
    close(im->lsock);
    im->lsock = -1;
    return -EAGAIN;
  }

  im->started = 1;
  MLOGI("mjpeg: serving on :%d", im->port);
  return 0;
}

static int mjpeg_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  mjpeg_impl_t *im = (mjpeg_impl_t *)self->impl;
  if (!im->started)
    return -ENOTCONN;

  /* Instrumentation headers (wire spec §1.2). t_sent is stamped here, right
   * before the writes, for the same reason as every other transport. */
  uint64_t t_sent = ucv_now_ns();
  char part[320];
  int  pn = snprintf(part, sizeof(part),
                     "--frame\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n"
                     "X-UCV-Seq: %u\r\n"
                     "X-UCV-Cap-Ns: %llu\r\n"
                     "X-UCV-Enc-Ns: %llu\r\n"
                     "X-UCV-Snd-Ns: %llu\r\n"
                     "X-UCV-Key: %d\r\n"
                     "X-UCV-Run: %u\r\n\r\n",
                     f->size, f->seq,
                     (unsigned long long)f->t_capture_ns,
                     (unsigned long long)f->t_encoded_ns,
                     (unsigned long long)t_sent,
                     f->is_keyframe ? 1 : 0,
                     ucv_get_run_id_hash());
  if (pn < 0 || pn >= (int)sizeof(part))
    return -EOVERFLOW;

  int delivered = 0;
  pthread_mutex_lock(&im->lock);
  for (int i = 0; i < MJPEG_MAX_CLIENTS; i++) {
    int s = im->clients[i];
    if (s < 0)
      continue;
    if (send_all_fd(s, (const uint8_t *)part, (size_t)pn) < 0 ||
        send_all_fd(s, f->data, f->size) < 0 ||
        send_all_fd(s, (const uint8_t *)"\r\n", 2) < 0) {
      MLOGI("mjpeg: client %d disconnected", i);
      close(s);
      im->clients[i] = -1;
      continue;
    }
    delivered++;
    self->bytes_wire += (uint64_t)pn + f->size + 2;
  }
  pthread_mutex_unlock(&im->lock);

  if (!delivered)
    return -ENOTCONN; /* no viewer attached — not an error, but not a send */

  self->frames_sent++;
  self->bytes_payload += f->size;
  return 0;
}

static void mjpeg_stop(ucv_transport_t *self) {
  mjpeg_impl_t *im = (mjpeg_impl_t *)self->impl;
  if (!im->started)
    return;
  im->quit = 1;
  if (im->lsock >= 0) {
    shutdown(im->lsock, SHUT_RDWR);
    close(im->lsock);
    im->lsock = -1;
  }
  pthread_join(im->accept_th, NULL);

  pthread_mutex_lock(&im->lock);
  for (int i = 0; i < MJPEG_MAX_CLIENTS; i++) {
    if (im->clients[i] >= 0) {
      close(im->clients[i]);
      im->clients[i] = -1;
    }
  }
  pthread_mutex_unlock(&im->lock);
  im->started = 0;
  MLOGI("mjpeg: stopped");
}

static mjpeg_impl_t g_mjpeg_impl = {
    .lsock = -1,
    .lock  = PTHREAD_MUTEX_INITIALIZER,
    .quit  = 1,
    .started = 0,
};

static ucv_transport_t g_mjpeg = {
    .name        = "mjpeg_http",
    .protocol_id = UCV_PROTO_MJPEG,
    .start       = mjpeg_start,
    .send        = mjpeg_send,
    .stop        = mjpeg_stop,
    .impl        = &g_mjpeg_impl,
};

ucv_transport_t *ucv_transport_mjpeg(void) { return &g_mjpeg; }
