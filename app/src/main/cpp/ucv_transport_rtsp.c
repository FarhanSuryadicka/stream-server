/* RTSP signalling (protocol #8) over the existing RTP/UDP data plane (#3).
 *
 * The RTP packetiser in ucv_transport_rtp.c is already RFC 6184 compliant and
 * carries the measurement header in an RFC 8285 extension. What it lacked was
 * signalling: a client could not discover the stream, negotiate ports, or start
 * and stop it. That is what this file adds, so the protocol can honestly be
 * called RTSP rather than "RTP with an out-of-band start" (handoff section 4).
 *
 * Shape: the phone is the RTSP SERVER (listener on 8554). A client — the
 * experiment receiver, ffplay, VLC or GStreamer — performs
 *
 *   OPTIONS -> DESCRIBE -> SETUP -> PLAY -> (RTP flows) -> TEARDOWN
 *
 * and the RTP packets then go to the transport port the client asked for in its
 * SETUP Transport header. Interoperability matters here: if a stock player
 * cannot play the stream, the label "RTSP" is not earned, so the SDP and the
 * response headers follow the RFC rather than a private shorthand.
 *
 * Only UDP unicast interleaving is offered. RTP-over-TCP (interleaved) is a
 * different measurement (it inherits TCP head-of-line blocking) and would
 * quietly change what the numbers mean, so it is refused with 461 rather than
 * silently accepted.
 */

#include "ucv_transport.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "ucv-rtsp"
#define SLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define SLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define RTSP_STREAM_PATH "ucv"
#define RTSP_REQUEST_MAX 4096

/* The RTP data plane this signalling layer drives. */
ucv_transport_t *ucv_transport_rtp(void);

typedef struct {
  int listen_sock;
  int client_sock;
  pthread_t thread;
  volatile int quit;
  volatile int playing;   /* set by PLAY, cleared by TEARDOWN */
  int  server_port;
  char session[24];
  char client_ip[48];
  char server_ip[48];
  int  client_rtp_port;
  ucv_transport_t *rtp;   /* started once the client has issued PLAY */
  int rtp_started;
  pthread_mutex_t lock;
  int started;
} rtsp_impl_t;

static rtsp_impl_t g_impl = {.listen_sock = -1, .client_sock = -1,
                             .lock = PTHREAD_MUTEX_INITIALIZER};

/* ---------------------------------------------------------------- */
/* Small request helpers                                             */
/* ---------------------------------------------------------------- */

/* Case-insensitive header lookup returning the value start, or NULL. */
static const char *header_value(const char *req, const char *name) {
  const size_t n = strlen(name);
  for (const char *p = req; *p; p++) {
    if ((p == req || p[-1] == '\n') && strncasecmp(p, name, n) == 0 &&
        p[n] == ':') {
      const char *v = p + n + 1;
      while (*v == ' ' || *v == '\t') v++;
      return v;
    }
  }
  return NULL;
}

static int header_int(const char *req, const char *name, int fallback) {
  const char *v = header_value(req, name);
  return v ? atoi(v) : fallback;
}

static int send_all_tcp(int sock, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = send(sock, buf + off, len - off, 0);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static void respond(int sock, int cseq, const char *status,
                    const char *extra_headers, const char *body) {
  char out[4096];
  int n;
  if (body) {
    /* Content-Length must match the body exactly or the client blocks waiting
     * for bytes that never arrive, so it is computed from the body itself. */
    n = snprintf(out, sizeof(out),
        "RTSP/1.0 %s\r\n"
        "CSeq: %d\r\n"
        "Server: ucv-transport-lab\r\n"
        "%s"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %zu\r\n"
        "\r\n%s",
        status, cseq, extra_headers ? extra_headers : "", strlen(body), body);
  } else {
    n = snprintf(out, sizeof(out),
        "RTSP/1.0 %s\r\n"
        "CSeq: %d\r\n"
        "Server: ucv-transport-lab\r\n"
        "%s"
        "\r\n",
        status, cseq, extra_headers ? extra_headers : "");
  }
  if (n > 0 && (size_t)n < sizeof(out)) send_all_tcp(sock, out, (size_t)n);
}

/* Builds the SDP for the current encoder settings.
 *
 * Deliberately minimal but standards-correct: a real player needs the payload
 * type, clock rate and packetization-mode to decode H.264 over RTP. SPS/PPS are
 * NOT advertised in sprop-parameter-sets because the encoder prepends codec
 * config to every keyframe (see ucv_encoder.h) — a player picks them up
 * in-band, and a stale sprop copied from a previous run would be worse than
 * none at all. */
static void build_sdp(char *out, size_t out_n, const char *server_ip,
                      int rtp_port) {
  snprintf(out, out_n,
      "v=0\r\n"
      "o=- 0 0 IN IP4 %s\r\n"
      "s=UCV Transport Lab\r\n"
      "i=UVC fisheye H.264, instrumented (RFC 8285 header extension)\r\n"
      "c=IN IP4 %s\r\n"
      "t=0 0\r\n"
      "a=tool:ucv-transport-lab\r\n"
      "a=sendonly\r\n"
      "m=video %d RTP/AVP 96\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n"
      "a=control:track0\r\n",
      server_ip, server_ip, rtp_port);
}

/* Extracts client_port=NNNN from a Transport header. Returns 0 on success. */
static int parse_client_port(const char *transport, int *port) {
  const char *p = transport ? strstr(transport, "client_port=") : NULL;
  if (!p) return -EINVAL;
  p += strlen("client_port=");
  const int v = atoi(p);
  if (v <= 0 || v > 65535) return -EINVAL;
  *port = v;
  return 0;
}

/* ---------------------------------------------------------------- */
/* Request handling                                                  */
/* ---------------------------------------------------------------- */

static void handle_request(rtsp_impl_t *im, int sock, const char *req) {
  char method[24] = {0}, uri[256] = {0};
  if (sscanf(req, "%23s %255s", method, uri) != 2) {
    respond(sock, 0, "400 Bad Request", NULL, NULL);
    return;
  }
  const int cseq = header_int(req, "CSeq", 0);

  if (strcmp(method, "OPTIONS") == 0) {
    respond(sock, cseq,  "200 OK",
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n", NULL);
    return;
  }

  if (strcmp(method, "DESCRIBE") == 0) {
    char sdp[768];
    const char *server_ip = im->server_ip[0] ? im->server_ip : "0.0.0.0";
    build_sdp(sdp, sizeof(sdp), server_ip,
              UCV_PORT_RTP);
    char extra[320];
    snprintf(extra, sizeof(extra), "Content-Base: rtsp://%s:%d/%s/\r\n",
             server_ip, im->server_port, RTSP_STREAM_PATH);
    respond(sock, cseq, "200 OK", extra, sdp);
    SLOGI("rtsp: DESCRIBE -> SDP sent");
    return;
  }

  if (strcmp(method, "SETUP") == 0) {
    const char *transport = header_value(req, "Transport");
    /* Interleaved (RTP over the RTSP TCP connection) would measure a different
     * transport entirely, so it is refused rather than silently accepted. */
    if (transport && strstr(transport, "TCP")) {
      respond(sock, cseq, "461 Unsupported Transport", NULL, NULL);
      SLOGE("rtsp: refused RTP/AVP/TCP - only UDP unicast is measured");
      return;
    }
    int client_port = 0;
    if (parse_client_port(transport, &client_port) != 0) {
      respond(sock, cseq, "461 Unsupported Transport", NULL, NULL);
      return;
    }
    pthread_mutex_lock(&im->lock);
    im->client_rtp_port = client_port;
    snprintf(im->session, sizeof(im->session), "%08X",
             (unsigned)(ucv_now_ns() & 0xffffffffu));
    pthread_mutex_unlock(&im->lock);

    char extra[320];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
             "Session: %s;timeout=60\r\n",
             client_port, client_port + 1, UCV_PORT_RTP, UCV_PORT_RTP + 1,
             im->session);
    respond(sock, cseq, "200 OK", extra, NULL);
    SLOGI("rtsp: SETUP client_port=%d session=%s", client_port, im->session);
    return;
  }

  if (strcmp(method, "PLAY") == 0) {
    if (!im->session[0]) {
      respond(sock, cseq, "454 Session Not Found", NULL, NULL);
      return;
    }
    /* Point the RTP data plane at the port the client negotiated, then start
     * it. The transport is shared with the plain rtp_udp protocol, so starting
     * it twice must be harmless — rtp_start() returns early when already up. */
    char peer[80];
    snprintf(peer, sizeof(peer), "%s:%d",
             im->client_ip[0] ? im->client_ip : "127.0.0.1",
             im->client_rtp_port);
    int rc = 0;
    pthread_mutex_lock(&im->lock);
    if (!im->rtp_started) {
      rc = im->rtp->start(im->rtp, peer);
      im->rtp_started = (rc == 0);
    }
    pthread_mutex_unlock(&im->lock);
    if (rc < 0) {
      respond(sock, cseq, "500 Internal Server Error", NULL, NULL);
      SLOGE("rtsp: PLAY could not start RTP to %s: %d", peer, rc);
      return;
    }
    im->playing = 1;
    char extra[80];
    snprintf(extra, sizeof(extra), "Session: %s\r\n", im->session);
    respond(sock, cseq, "200 OK", extra, NULL);
    SLOGI("rtsp: PLAY -> RTP to %s", peer);
    return;
  }

  if (strcmp(method, "TEARDOWN") == 0) {
    im->playing = 0;
    char closed_session[sizeof(im->session)];
    pthread_mutex_lock(&im->lock);
    snprintf(closed_session, sizeof(closed_session), "%s", im->session);
    if (im->rtp_started) { im->rtp->stop(im->rtp); im->rtp_started = 0; }
    im->session[0] = '\0';
    pthread_mutex_unlock(&im->lock);
    char extra[64];
    snprintf(extra, sizeof(extra), "Session: %s\r\n", closed_session);
    respond(sock, cseq, "200 OK", extra, NULL);
    SLOGI("rtsp: TEARDOWN");
    return;
  }

  respond(sock, cseq, "501 Not Implemented", NULL, NULL);
}

/* Serves one client at a time: this is a measurement rig, not a CDN, and a
 * second viewer would change the very bandwidth being measured. */
static void *rtsp_thread(void *arg) {
  rtsp_impl_t *im = (rtsp_impl_t *)arg;
  SLOGI("rtsp: listening on :%d/%s", im->server_port, RTSP_STREAM_PATH);
  while (!im->quit) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int cs = accept(im->listen_sock, (struct sockaddr *)&peer, &peer_len);
    if (cs < 0) {
      if (im->quit) break;
      continue;
    }
    inet_ntop(AF_INET, &peer.sin_addr, im->client_ip, sizeof(im->client_ip));
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(cs, (struct sockaddr *)&local, &local_len) == 0)
      inet_ntop(AF_INET, &local.sin_addr, im->server_ip, sizeof(im->server_ip));
    im->client_sock = cs;
    SLOGI("rtsp: client %s connected", im->client_ip);

    char req[RTSP_REQUEST_MAX];
    size_t used = 0;
    while (!im->quit) {
      const ssize_t n = recv(cs, req + used, sizeof(req) - used - 1, 0);
      if (n <= 0) break;
      used += (size_t)n;
      req[used] = '\0';
      /* Requests are processed once the blank line terminating the headers has
       * arrived; RTSP has no body on the methods handled here. */
      char *end;
      while ((end = strstr(req, "\r\n\r\n")) != NULL) {
        const size_t len = (size_t)(end - req) + 4;
        char one[RTSP_REQUEST_MAX];
        memcpy(one, req, len);
        one[len] = '\0';
        handle_request(im, cs, one);
        memmove(req, req + len, used - len + 1);
        used -= len;
      }
      if (used >= sizeof(req) - 1) used = 0; /* oversized junk, resynchronise */
    }
    close(cs);
    im->client_sock = -1;
    im->playing = 0;
    pthread_mutex_lock(&im->lock);
    if (im->rtp_started) { im->rtp->stop(im->rtp); im->rtp_started = 0; }
    im->session[0] = '\0';
    pthread_mutex_unlock(&im->lock);
    SLOGI("rtsp: client disconnected");
  }
  SLOGI("rtsp: listener exiting");
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Transport interface                                               */
/* ---------------------------------------------------------------- */

/* cfg is "<peer-ip>:<port>" like the other transports. The port is the RTSP
 * signalling port to listen on; the peer IP is only a hint, since the actual
 * client address is taken from the accepted connection. */
static int rtsp_start(ucv_transport_t *self, const char *cfg) {
  rtsp_impl_t *im = (rtsp_impl_t *)self->impl;
  if (im->started) return 0;
  im->rtp = ucv_transport_rtp();
  if (!im->rtp) return -ENOSYS;

  im->server_port = UCV_PORT_RTSP;
  const char *colon = cfg ? strrchr(cfg, ':') : NULL;
  if (colon) {
    const int p = atoi(colon + 1);
    if (p > 0 && p <= 65535) im->server_port = p;
    const size_t host_n = (size_t)(colon - cfg);
    if (host_n && host_n < sizeof(im->client_ip)) {
      memcpy(im->client_ip, cfg, host_n);
      im->client_ip[host_n] = '\0';
    }
  }

  im->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (im->listen_sock < 0) return -errno;
  int one = 1;
  setsockopt(im->listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)im->server_port);
  if (bind(im->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    const int e = errno;
    close(im->listen_sock); im->listen_sock = -1;
    SLOGE("rtsp: bind :%d failed errno=%d", im->server_port, e);
    return -e;
  }
  listen(im->listen_sock, 2);
  im->quit = 0;
  im->playing = 0;
  im->rtp_started = 0;
  im->session[0] = '\0';
  im->server_ip[0] = '\0';
  im->rtp->frames_sent = 0;
  im->rtp->bytes_payload = 0;
  im->rtp->bytes_wire = 0;
  im->rtp->send_errors = 0;
  if (pthread_create(&im->thread, NULL, rtsp_thread, im) != 0) {
    close(im->listen_sock); im->listen_sock = -1;
    return -EAGAIN;
  }
  im->started = 1;
  return 0;
}

/* Frames are dropped until the client has issued PLAY. That is correct RTSP
 * behaviour, and counting them as send errors would make the loss statistics
 * describe the handshake rather than the network. */
static int rtsp_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  rtsp_impl_t *im = (rtsp_impl_t *)self->impl;
  if (!im->started) return -ENOTCONN;
  if (!im->playing || !im->rtp_started) return 0;
  const int rc = im->rtp->send(im->rtp, f);
  /* The RTP transport keeps its own counters; mirror them so the run summary
   * reports one consistent set regardless of which entry point was used. */
  self->frames_sent   = im->rtp->frames_sent;
  self->bytes_payload = im->rtp->bytes_payload;
  self->bytes_wire    = im->rtp->bytes_wire;
  self->send_errors   = im->rtp->send_errors;
  return rc;
}

static void rtsp_stop(ucv_transport_t *self) {
  rtsp_impl_t *im = (rtsp_impl_t *)self->impl;
  if (!im->started) return;
  im->quit = 1;
  im->playing = 0;
  if (im->listen_sock >= 0) {
    shutdown(im->listen_sock, SHUT_RDWR);
    close(im->listen_sock);
    im->listen_sock = -1;
  }
  if (im->client_sock >= 0) shutdown(im->client_sock, SHUT_RDWR);
  pthread_join(im->thread, NULL);
  pthread_mutex_lock(&im->lock);
  if (im->rtp_started) { im->rtp->stop(im->rtp); im->rtp_started = 0; }
  pthread_mutex_unlock(&im->lock);
  im->started = 0;
}

static ucv_transport_t g_rtsp = {
    .name = "rtsp", .protocol_id = UCV_PROTO_RTSP_SIGNALLED,
    .start = rtsp_start, .send = rtsp_send, .stop = rtsp_stop,
    .impl = &g_impl,
};

ucv_transport_t *ucv_transport_rtsp_signalled(void) { return &g_rtsp; }
