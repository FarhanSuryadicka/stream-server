/* WebRTC transport (protocol #1) via libdatachannel.
 *
 * The protocol the issue expects to win, because it is the only one here with a
 * genuine native low-latency back-channel (RTCDataChannel) rather than a paired
 * side socket. That property is what harness spec §7.3 says decides usability,
 * so it has to be measured rather than assumed.
 *
 * C++ rather than C because libdatachannel is a C++ library; the transport
 * interface it exports stays plain C so ucv_pipeline.c is unchanged.
 *
 * ## Media path: no re-encode
 *
 * The already-encoded H.264 access units from ucv_encoder.c are handed straight
 * to an H264RtpPacketizer with Separator::StartSequence (MediaCodec emits
 * Annex-B). libdatachannel is NEVER allowed to encode anything: the frozen
 * encoder configuration in harness spec §2 is what makes the seven protocols
 * comparable, and letting a library re-encode would silently void it — the
 * failure implementation plan §6 warns about explicitly.
 *
 * ## Instrumentation: over the data channel, deliberately
 *
 * libdatachannel exposes no generic RTP header extension (only MID/RID/playout
 * delay), so the UCV measurement header cannot ride in the RTP header the way it
 * does for the plain RTP transport. It travels on the data channel instead,
 * which has two advantages beyond necessity:
 *
 *   - the media track stays standards-clean, so a browser can still play it and
 *     interoperability is provable rather than claimed;
 *   - it exercises the very back-channel §7.3 wants measured.
 *
 * t_sent_ns is stamped immediately before the media write, and the metadata
 * carrying it is sent straight after. Metadata may therefore arrive after its
 * frame — harmless, because the receiver pairs by sequence number, exactly like
 * the HLS sidecar.
 *
 * ## Signalling
 *
 * A minimal length-prefixed SDP exchange over plain TCP, owned by this project
 * rather than taken from libdatachannel's WebSocket support. WebRTC is the only
 * protocol in the comparison that needs a rendezvous server at all, and that is
 * a real deployment cost the operational-complexity column has to record, so it
 * is kept visible instead of hidden inside a dependency.
 */

#include "ucv_transport.h"

#include <rtc/rtc.hpp>

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define TAG "ucv-webrtc"
#define WLOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define WLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr uint8_t  kPayloadType = 96;
constexpr uint32_t kClockRate   = 90000;

struct WebrtcImpl {
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track>          track;
  std::shared_ptr<rtc::DataChannel>    meta;
  std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config;

  std::mutex              lock;
  std::condition_variable cv;
  bool     media_open   = false;
  bool     failed       = false;
  uint32_t packet_seq   = 0;
  bool     started      = false;
};

WebrtcImpl g_impl;

/* ---------------------------------------------------------------- */
/* Signalling: length-prefixed SDP over TCP                          */
/* ---------------------------------------------------------------- */

bool send_all(int s, const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(s, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool recv_line(int s, std::string *out) {
  out->clear();
  for (;;) {
    char c;
    const ssize_t n = ::recv(s, &c, 1, 0);
    if (n <= 0) return false;
    if (c == '\n') return true;
    out->push_back(c);
    if (out->size() > 64) return false;
  }
}

bool recv_exact(int s, std::string *out, size_t len) {
  out->assign(len, '\0');
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::recv(s, &(*out)[off], len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

/* Exchanges the local offer for the peer's answer. Returns 0 on success. */
int signal_exchange(const std::string &host, int port,
                    const std::string &offer, std::string *answer) {
  const int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return -errno;
  int one = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(s);
    return -EINVAL;
  }
  if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(s);
    WLOGE("webrtc: signalling connect %s:%d failed errno=%d",
          host.c_str(), port, e);
    return -e;
  }

  char head[64];
  const int hn = std::snprintf(head, sizeof(head), "OFFER %zu\n", offer.size());
  if (!send_all(s, head, static_cast<size_t>(hn)) ||
      !send_all(s, offer.data(), offer.size())) {
    ::close(s);
    return -EIO;
  }

  std::string line;
  if (!recv_line(s, &line)) { ::close(s); return -EIO; }
  size_t len = 0;
  if (std::sscanf(line.c_str(), "ANSWER %zu", &len) != 1 ||
      len == 0 || len > 256 * 1024) {
    ::close(s);
    WLOGE("webrtc: bad signalling reply: %s", line.c_str());
    return -EPROTO;
  }
  if (!recv_exact(s, answer, len)) { ::close(s); return -EIO; }
  ::close(s);
  return 0;
}

/* ---------------------------------------------------------------- */
/* Transport interface                                               */
/* ---------------------------------------------------------------- */

/* cfg is "<pc-ip>:<signalling-port>". */
int webrtc_start(ucv_transport_t *self, const char *cfg) {
  auto *im = static_cast<WebrtcImpl *>(self->impl);
  if (im->started) return 0;

  std::string host;
  int port = UCV_PORT_SIGNAL;
  if (cfg && *cfg) {
    const char *colon = std::strrchr(cfg, ':');
    if (colon) {
      host.assign(cfg, static_cast<size_t>(colon - cfg));
      const int p = std::atoi(colon + 1);
      if (p > 0 && p <= 65535) port = p;
    } else {
      host = cfg;
    }
  }
  if (host.empty()) return -EINVAL;

  im->media_open = false;
  im->failed = false;
  im->packet_seq = 0;

  rtc::Configuration config;
  /* No STUN/TURN: the rig is a single LAN behind one router (runbook §2), so
   * host candidates are sufficient. Adding a public STUN server would put an
   * internet round trip inside connection setup and make the numbers depend on
   * something outside the experiment. Record this in the report — it is why
   * these WebRTC figures are a LAN best case, not a NAT-traversal result. */
  config.disableAutoNegotiation = false;

  try {
    im->pc = std::make_shared<rtc::PeerConnection>(config);
  } catch (const std::exception &e) {
    WLOGE("webrtc: PeerConnection failed: %s", e.what());
    return -EIO;
  }

  im->pc->onStateChange([im](rtc::PeerConnection::State state) {
    WLOGI("webrtc: pc state %d", static_cast<int>(state));
    if (state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Closed) {
      std::lock_guard<std::mutex> g(im->lock);
      im->failed = true;
      im->cv.notify_all();
    }
  });

  /* Send-only H.264 video. */
  rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
  media.addH264Codec(kPayloadType);
  media.addSSRC(42, "ucv-video");

  try {
    im->track = im->pc->addTrack(media);
    im->rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        42, "ucv-video", kPayloadType, kClockRate);
    /* StartSequence covers both 3- and 4-byte Annex-B start codes, which is what
     * MediaCodec emits and what the encoder prepends codec config with. */
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, im->rtp_config);
    im->track->setMediaHandler(packetizer);
    im->track->onOpen([im]() {
      WLOGI("webrtc: media track open");
      std::lock_guard<std::mutex> g(im->lock);
      im->media_open = true;
      im->cv.notify_all();
    });

    /* The instrumentation channel. Ordered and reliable so no measurement
     * record is silently lost — losing a frame is a finding, losing the record
     * OF a frame is just a hole in the data. */
    im->meta = im->pc->createDataChannel("ucv-meta");
  } catch (const std::exception &e) {
    WLOGE("webrtc: track setup failed: %s", e.what());
    im->pc.reset();
    return -EIO;
  }

  /* Gather candidates, then offer. Waiting for gathering to complete keeps the
   * signalling to a single round trip (no trickle ICE), which is all a LAN rig
   * needs and keeps the exchange easy to reason about. */
  std::string offer;
  {
    std::mutex m;
    std::condition_variable cvg;
    bool done = false;
    im->pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
      if (s == rtc::PeerConnection::GatheringState::Complete) {
        std::lock_guard<std::mutex> g(m);
        done = true;
        cvg.notify_all();
      }
    });
    im->pc->setLocalDescription();
    std::unique_lock<std::mutex> g(m);
    cvg.wait_for(g, std::chrono::seconds(5), [&] { return done; });
  }
  if (auto local = im->pc->localDescription()) {
    offer = std::string(local.value());
  }
  if (offer.empty()) {
    WLOGE("webrtc: no local description produced");
    im->pc.reset();
    return -EIO;
  }

  std::string answer;
  const int rc = signal_exchange(host, port, offer, &answer);
  if (rc != 0) {
    im->pc.reset();
    return rc;
  }

  try {
    im->pc->setRemoteDescription(rtc::Description(answer, "answer"));
  } catch (const std::exception &e) {
    WLOGE("webrtc: bad answer: %s", e.what());
    im->pc.reset();
    return -EPROTO;
  }

  /* Wait for the track to open before declaring the transport up; sending into
   * a half-open peer connection would count as send errors that belong to setup
   * rather than to the network. */
  {
    std::unique_lock<std::mutex> g(im->lock);
    im->cv.wait_for(g, std::chrono::seconds(15),
                    [im] { return im->media_open || im->failed; });
    if (!im->media_open) {
      WLOGE("webrtc: media track did not open (failed=%d)", im->failed ? 1 : 0);
      g.unlock();
      im->pc.reset();
      return -ETIMEDOUT;
    }
  }

  im->started = true;
  WLOGI("webrtc: connected, signalling via %s:%d", host.c_str(), port);
  return 0;
}

int webrtc_send(ucv_transport_t *self, const ucv_encoded_frame_t *f) {
  auto *im = static_cast<WebrtcImpl *>(self->impl);
  if (!im->started) return -ENOTCONN;
  if (im->failed) return -ENOTCONN;
  if (!im->track || !im->track->isOpen()) return -ENOTCONN;

  /* RTP timestamp from the capture clock, at the 90 kHz video rate. */
  im->rtp_config->timestamp =
      static_cast<uint32_t>((f->t_capture_ns / 100000ull) * 9ull);

  ucv_frame_header_t h;
  ucv_fill_header(&h, f, UCV_PROTO_WEBRTC, static_cast<uint32_t>(f->size),
                  f->is_keyframe ? UCV_FLAG_KEYFRAME : 0);
  h.packet_seq = im->packet_seq++;
  h.fragment_index = 0;
  h.fragment_count = 1;

  bool ok = false;
  try {
    /* Stamped as late as possible: anything earlier folds application queueing
     * into the measured transport latency (see ucv_transport.h). */
    h.t_sent_ns = ucv_now_ns();
    ucv_frame_header_finalize(&h);
    ok = im->track->send(reinterpret_cast<const std::byte *>(f->data), f->size);
  } catch (const std::exception &e) {
    WLOGE("webrtc: track send threw: %s", e.what());
    ok = false;
  }
  if (!ok) {
    self->send_errors++;
    return -EIO;
  }

  /* Instrumentation follows the media, carrying the t_sent stamped above. */
  if (im->meta && im->meta->isOpen()) {
    try {
      im->meta->send(reinterpret_cast<const std::byte *>(&h), sizeof(h));
    } catch (const std::exception &) {
      /* A dropped metadata record leaves a hole in the data but does not
       * invalidate the media path, so it is not a transport send error. */
    }
  }

  self->frames_sent++;
  self->bytes_payload += f->size;
  /* Wire bytes are not observable from here: SRTP encryption, RTP headers and
   * SCTP framing all happen below this API. Reported as payload so the run does
   * not claim a wire figure it cannot measure; the report must say a packet
   * capture is needed for WebRTC overhead. */
  self->bytes_wire += f->size;
  return 0;
}

void webrtc_stop(ucv_transport_t *self) {
  auto *im = static_cast<WebrtcImpl *>(self->impl);
  if (!im->started) return;
  try {
    if (im->meta) im->meta->close();
    if (im->track) im->track->close();
    if (im->pc) im->pc->close();
  } catch (const std::exception &) {
    /* Teardown races with libdatachannel's own threads; nothing useful to do. */
  }
  im->meta.reset();
  im->track.reset();
  im->rtp_config.reset();
  im->pc.reset();
  im->media_open = false;
  im->started = false;
}

ucv_transport_t g_webrtc = {
    /* name */            "webrtc",
    /* protocol_id */     UCV_PROTO_WEBRTC,
    /* start */           webrtc_start,
    /* send */            webrtc_send,
    /* stop */            webrtc_stop,
    /* impl */            &g_impl,
    /* frames_sent */     0,
    /* bytes_payload */   0,
    /* bytes_wire */      0,
    /* send_errors */     0,
};

}  // namespace

extern "C" ucv_transport_t *ucv_transport_webrtc(void) { return &g_webrtc; }
