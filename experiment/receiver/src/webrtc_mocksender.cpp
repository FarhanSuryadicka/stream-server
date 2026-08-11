// ucv-webrtc-mocksender — phone stand-in for the WebRTC path.
//
// Speaks the exact protocol ucv_transport_webrtc.cpp speaks: the same
// length-prefixed SDP offer over TCP, an H.264 media track packetised with
// Separator::StartSequence, and per-frame ucv_frame_header_t records over an
// "ucv-meta" data channel. That lets the whole WebRTC measurement path be
// validated on one machine, before a phone and camera are attached.
//
// It is a TEST INSTRUMENT, not a protocol implementation. Numbers measured
// against it describe the loopback path, never WebRTC — the same caveat that
// applies to ucv-mocksender. They must not enter the results table.
//
// Kept separate from the Android transport rather than compiling that file for
// the PC: the transport is Android-native (android/log.h, POSIX sockets), and
// shimming it for Windows was changing more of the code under test than the
// test was worth. The Android transport is verified by compiling and linking
// for arm64 and by the on-device bring-up; this exercises the receiver.

#include "ucv_wire.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr uint8_t  kPayloadType = 96;
constexpr uint32_t kClockRate   = 90000;

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool SendAllTcp(int s, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t off = 0;
  while (off < len) {
#ifdef _WIN32
    const int n = ::send(static_cast<SOCKET>(s), p + off,
                         static_cast<int>(len - off), 0);
#else
    const ssize_t n = ::send(s, p + off, len - off, 0);
#endif
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool RecvLineTcp(int s, std::string* out) {
  out->clear();
  for (;;) {
    char c;
#ifdef _WIN32
    const int n = ::recv(static_cast<SOCKET>(s), &c, 1, 0);
#else
    const ssize_t n = ::recv(s, &c, 1, 0);
#endif
    if (n <= 0) return false;
    if (c == '\n') return true;
    out->push_back(c);
    if (out->size() > 64) return false;
  }
}

bool RecvExactTcp(int s, std::string* out, size_t len) {
  out->assign(len, '\0');
  size_t off = 0;
  while (off < len) {
#ifdef _WIN32
    const int n = ::recv(static_cast<SOCKET>(s), &(*out)[off],
                         static_cast<int>(len - off), 0);
#else
    const ssize_t n = ::recv(s, &(*out)[off], len - off, 0);
#endif
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

void CloseSock(int s) {
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(s));
#else
  ::close(s);
#endif
}

int SignalExchange(const std::string& host, int port, const std::string& offer,
                   std::string* answer) {
  const int s = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (s < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    CloseSock(s);
    return -1;
  }
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSock(s);
    return -1;
  }
  char head[64];
  const int hn = std::snprintf(head, sizeof(head), "OFFER %zu\n", offer.size());
  if (!SendAllTcp(s, head, static_cast<size_t>(hn)) ||
      !SendAllTcp(s, offer.data(), offer.size())) {
    CloseSock(s);
    return -1;
  }
  std::string line;
  if (!RecvLineTcp(s, &line)) { CloseSock(s); return -1; }
  size_t len = 0;
  if (std::sscanf(line.c_str(), "ANSWER %zu", &len) != 1 || !len ||
      len > 256 * 1024) {
    std::fprintf(stderr, "mock: bad answer header: %s\n", line.c_str());
    CloseSock(s);
    return -1;
  }
  if (!RecvExactTcp(s, answer, len)) { CloseSock(s); return -1; }
  CloseSock(s);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  std::string host = "127.0.0.1";
  int port = UCV_PORT_SIGNAL;
  int duration = 12;
  int fps = 30;
  std::string run_id = "WEBRTC-MOCK";

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--host")     { if (auto v = val()) host = v; }
    else if (a == "--port")     { if (auto v = val()) port = std::atoi(v); }
    else if (a == "--duration") { if (auto v = val()) duration = std::atoi(v); }
    else if (a == "--fps")      { if (auto v = val()) fps = std::atoi(v); }
    else if (a == "--run-id")   { if (auto v = val()) run_id = v; }
    else {
      std::printf("ucv-webrtc-mocksender — phone stand-in for the WebRTC path\n"
                  "  --host <ip>       signalling host (default 127.0.0.1)\n"
                  "  --port <n>        signalling port (default %d)\n"
                  "  --duration <s>    stream length  (default 12)\n"
                  "  --fps <n>         frame rate     (default 30)\n"
                  "  --run-id <id>     must match the receiver's --run-id\n"
                  "\nA TEST INSTRUMENT. Its numbers describe loopback, not WebRTC.\n",
                  UCV_PORT_SIGNAL);
      return a == "--help" ? 0 : 1;
    }
  }

  const uint32_t run_hash = ucv_run_id_hash(run_id.c_str());

  rtc::Configuration config;
  auto pc = std::make_shared<rtc::PeerConnection>(config);

  std::mutex m;
  std::condition_variable cv;
  bool media_open = false, failed = false;

  pc->onStateChange([&](rtc::PeerConnection::State state) {
    if (state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Closed) {
      std::lock_guard<std::mutex> g(m);
      failed = true;
      cv.notify_all();
    }
  });

  rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
  media.addH264Codec(kPayloadType);
  media.addSSRC(42, "ucv-video");
  auto track = pc->addTrack(media);
  auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
      42, "ucv-video", kPayloadType, kClockRate);
  track->setMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(
      rtc::NalUnit::Separator::StartSequence, rtp_config));
  track->onOpen([&]() {
    std::lock_guard<std::mutex> g(m);
    media_open = true;
    cv.notify_all();
  });
  auto meta = pc->createDataChannel("ucv-meta");

  {
    std::mutex gm;
    std::condition_variable gcv;
    bool done = false;
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
      if (s == rtc::PeerConnection::GatheringState::Complete) {
        std::lock_guard<std::mutex> g(gm);
        done = true;
        gcv.notify_all();
      }
    });
    pc->setLocalDescription();
    std::unique_lock<std::mutex> g(gm);
    gcv.wait_for(g, std::chrono::seconds(5), [&] { return done; });
  }

  std::string offer;
  if (auto local = pc->localDescription()) offer = std::string(local.value());
  if (offer.empty()) {
    std::fprintf(stderr, "mock: no local description\n");
    return 1;
  }

  std::string answer;
  // The receiver may not be listening yet when a script starts both at once.
  int rc = -1;
  for (int i = 0; i < 40 && rc != 0; i++) {
    rc = SignalExchange(host, port, offer, &answer);
    if (rc != 0) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (rc != 0) {
    std::fprintf(stderr, "mock: signalling to %s:%d failed\n", host.c_str(), port);
    return 1;
  }
  pc->setRemoteDescription(rtc::Description(answer, "answer"));

  {
    std::unique_lock<std::mutex> g(m);
    if (!cv.wait_for(g, std::chrono::seconds(20),
                     [&] { return media_open || failed; }) || !media_open) {
      std::fprintf(stderr, "mock: media track did not open\n");
      return 1;
    }
  }
  std::printf("mock: WebRTC connected, streaming %d s at %d fps\n", duration, fps);

  std::vector<std::byte> au(60000);
  const int total = duration * fps;
  uint32_t packet_seq = 0;
  int sent = 0, errors = 0;

  for (int i = 0; i < total; i++) {
    size_t n = 0;
    const bool key = (i % 30 == 0);
    auto put = [&](uint8_t b) { au[n++] = static_cast<std::byte>(b); };
    if (key) {
      // SPS then PPS, as MediaCodec prepends to every keyframe.
      put(0); put(0); put(0); put(1); put(0x67);
      put(0x42); put(0xc0); put(0x1f);
      for (int j = 0; j < 6; j++) put(static_cast<uint8_t>(j + 2));
      put(0); put(0); put(0); put(1); put(0x68);
      for (int j = 0; j < 4; j++) put(static_cast<uint8_t>(j + 1));
    }
    put(0); put(0); put(0); put(1); put(key ? 0x65 : 0x41);
    const size_t body = key ? 12000 : 5000;
    for (size_t j = 0; j < body; j++)
      put(static_cast<uint8_t>(j * 7 + i));

    const uint64_t t_capture = NowNs();
    const uint64_t t_encoded = t_capture + 8000000ull;   // 8 ms synthetic encode
    rtp_config->timestamp =
        static_cast<uint32_t>((t_capture / 100000ull) * 9ull);

    ucv_frame_header_t h{};
    h.protocol_id = UCV_PROTO_WEBRTC;
    h.frame_seq = static_cast<uint32_t>(i + 1);
    h.packet_seq = packet_seq++;
    h.fragment_index = 0;
    h.fragment_count = 1;
    h.t_capture_ns = t_capture;
    h.t_encoded_ns = t_encoded;
    h.payload_bytes = static_cast<uint32_t>(n);
    h.flags = key ? UCV_FLAG_KEYFRAME : 0;
    h.run_id_hash = run_hash;

    bool ok = false;
    try {
      h.t_sent_ns = NowNs();
      ucv_frame_header_finalize(&h);
      ok = track->send(au.data(), n);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "mock: send threw: %s\n", e.what());
    }
    if (ok) {
      sent++;
      if (meta && meta->isOpen()) {
        try {
          meta->send(reinterpret_cast<const std::byte*>(&h), sizeof(h));
        } catch (const std::exception&) { /* metadata hole only */ }
      }
    } else {
      errors++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
  }

  std::printf("mock: sent=%d errors=%d\n", sent, errors);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  try {
    if (meta) meta->close();
    track->close();
    pc->close();
  } catch (const std::exception&) {}
  return 0;
}
