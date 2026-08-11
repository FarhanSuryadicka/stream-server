// ucv-mocksender — a phone stand-in for validating the harness itself.
//
// Speaks the exact wire protocol the Android app speaks: answers PING so the
// receiver can synchronise clocks, ACKs control commands, and emits fake
// H.264-sized frames over Raw UDP with correct instrumentation.
//
// Why this exists: the harness must be proven correct BEFORE it is used to
// judge protocols, and it cannot be proven correct on a rig that needs a
// phone, a fisheye camera and a router to be present. With this, the whole
// measurement path — CRC, fragmentation, reassembly, clock sync, percentiles,
// loss accounting — is testable on one machine.
//
// It is a test instrument, NOT a protocol implementation. Numbers produced
// against it describe the loopback path, not any real protocol, and must
// never appear in the results table.

#include "ucv_wire.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t = int;
  #define CLOSESOCK closesocket
  using Sock = unsigned long long;
  static const Sock kBad = ~0ull;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define CLOSESOCK close
  using Sock = int;
  static const Sock kBad = -1;
#endif

namespace {

std::atomic<bool> g_quit{false};

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int SelectNfds(Sock s) {
#ifdef _WIN32
  (void)s; return 0;
#else
  return s + 1;
#endif
}

// Answers PING (enabling clock sync) and ACKs control commands, exactly as
// ucv_control.c does on the phone.
void ControlResponder(int port) {
  Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBad) { std::fprintf(stderr, "mock: control socket failed\n"); return; }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::fprintf(stderr, "mock: control bind :%d failed\n", port);
    CLOSESOCK(s);
    return;
  }
  std::printf("mock: control responder on :%d\n", port);

  uint64_t answered = 0;
  while (!g_quit) {
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    timeval tv{0, 200000};
    if (select(SelectNfds(s), &rfds, nullptr, nullptr, &tv) <= 0) continue;

    ucv_control_t cmd{};
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    const int n = recvfrom(s, reinterpret_cast<char*>(&cmd), sizeof(cmd), 0,
                           reinterpret_cast<sockaddr*>(&from), &fromlen);
    // t2 stamped immediately, as the phone does.
    const uint64_t t_recv = NowNs();
    if (n != static_cast<int>(sizeof(cmd))) continue;
    if (!ucv_control_valid(&cmd)) continue;

    if (cmd.flags & UCV_CTL_ACK_REQUESTED) {
      ucv_ack_t ack{};
      ack.cmd_type      = cmd.cmd_type;
      ack.flags         = UCV_ACK_APPLIED;
      ack.cmd_seq       = cmd.cmd_seq;
      ack.t_cmd_sent_ns = cmd.t_sent_ns;
      ack.t_recv_ns     = t_recv;
      ack.t_ack_ns      = NowNs();  // t3, stamped last
      ucv_ack_finalize(&ack);
      sendto(s, reinterpret_cast<const char*>(&ack), sizeof(ack), 0,
             reinterpret_cast<sockaddr*>(&from), fromlen);
      answered++;
    }
  }
  std::printf("mock: control responder stopped (%llu answered)\n",
              static_cast<unsigned long long>(answered));
  CLOSESOCK(s);
}

}  // namespace

int main(int argc, char** argv) {
  std::string pc_ip     = "127.0.0.1";
  std::string run_id;
  int  video_port   = UCV_PORT_RAWUDP;
  int  control_port = UCV_PORT_CONTROL;
  int  fps          = 30;
  int  duration_s   = 30;
  int  frame_bytes  = 16000;   // ~4 Mbps at 30 fps
  int  key_every    = 30;      // 1 s GOP, matching the frozen config
  int  drop_permil  = 0;       // synthetic loss, per mille

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (a == "--pc")            { if (auto v = val()) pc_ip = v; }
    else if (a == "--run-id")   { if (auto v = val()) run_id = v; }
    else if (a == "--video-port")   { if (auto v = val()) video_port = std::atoi(v); }
    else if (a == "--control-port") { if (auto v = val()) control_port = std::atoi(v); }
    else if (a == "--fps")      { if (auto v = val()) fps = std::atoi(v); }
    else if (a == "--duration") { if (auto v = val()) duration_s = std::atoi(v); }
    else if (a == "--bytes")    { if (auto v = val()) frame_bytes = std::atoi(v); }
    else if (a == "--drop-permil") { if (auto v = val()) drop_permil = std::atoi(v); }
    else if (a == "--help" || a == "-h") {
      std::printf(
          "ucv-mocksender — phone stand-in for validating the harness\n\n"
          "  --pc <ip>            Receiver address (default 127.0.0.1)\n"
          "  --run-id <id>        MUST match the receiver's --run-id\n"
          "  --video-port <n>     default %d\n"
          "  --control-port <n>   default %d\n"
          "  --fps <n>            default 30\n"
          "  --duration <s>       default 30\n"
          "  --bytes <n>          bytes per frame (default 16000)\n"
          "  --drop-permil <n>    synthetic loss per mille, to prove the\n"
          "                       receiver's loss accounting actually works\n\n"
          "NOT a protocol implementation. Numbers measured against it describe\n"
          "the loopback path only and must never enter the results table.\n",
          UCV_PORT_RAWUDP, UCV_PORT_CONTROL);
      return 0;
    }
  }

  if (run_id.empty()) {
    std::fprintf(stderr,
                 "error: --run-id is required and must match the receiver's,\n"
                 "       otherwise the receiver correctly discards every frame\n"
                 "       as belonging to a different run.\n");
    return 1;
  }

#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif

  std::thread control(ControlResponder, control_port);

  Sock v = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (v == kBad) { std::fprintf(stderr, "mock: video socket failed\n"); return 1; }

  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port   = htons(static_cast<uint16_t>(video_port));
  if (inet_pton(AF_INET, pc_ip.c_str(), &peer.sin_addr) != 1) {
    std::fprintf(stderr, "mock: bad --pc address '%s'\n", pc_ip.c_str());
    return 1;
  }

  const uint32_t run_hash = ucv_run_id_hash(run_id.c_str());
  std::printf("mock: streaming to %s:%d  run_id=%s  %d fps, %d B/frame\n",
              pc_ip.c_str(), video_port, run_id.c_str(), fps, frame_bytes);

  std::vector<uint8_t> payload(static_cast<size_t>(frame_bytes), 0xAB);
  std::vector<uint8_t> pkt(UCV_FRAME_HEADER_SIZE + UCV_UDP_FRAGMENT_PAYLOAD);

  const auto period = std::chrono::nanoseconds(1'000'000'000LL / (fps > 0 ? fps : 30));
  const auto t_end  = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
  auto next = std::chrono::steady_clock::now();

  uint32_t seq = 0;
  uint32_t packet_seq = 0;
  uint64_t sent_frames = 0, dropped_frames = 0;
  unsigned rng = 12345;

  while (std::chrono::steady_clock::now() < t_end) {
    std::this_thread::sleep_until(next);
    next += period;

    const uint64_t t_cap = NowNs();
    // Pretend the encoder took ~8 ms — a plausible 720p hardware figure, so
    // the capture→encode column shows something realistic.
    const uint64_t t_enc = t_cap + 8'000'000;
    const bool key = (seq % static_cast<uint32_t>(key_every)) == 0;

    // Deterministic synthetic loss, to prove the receiver's gap accounting
    // reports what actually went missing.
    rng = rng * 1103515245u + 12345u;
    if (drop_permil > 0 && static_cast<int>((rng >> 16) % 1000u) < drop_permil) {
      dropped_frames++;
      packet_seq += static_cast<uint32_t>(
          (payload.size() + UCV_UDP_FRAGMENT_PAYLOAD - 1) /
          UCV_UDP_FRAGMENT_PAYLOAD);
      seq++;
      continue;
    }

    size_t offset = 0;
    uint16_t fragment_index = 0;
    const uint16_t fragment_count = static_cast<uint16_t>(
        (payload.size() + UCV_UDP_FRAGMENT_PAYLOAD - 1) /
        UCV_UDP_FRAGMENT_PAYLOAD);
    do {
      size_t chunk = payload.size() - offset;
      uint8_t flags = key ? UCV_FLAG_KEYFRAME : 0;
      if (payload.size() > UCV_UDP_FRAGMENT_PAYLOAD) {
        flags |= UCV_FLAG_FRAGMENTED;
        if (chunk > UCV_UDP_FRAGMENT_PAYLOAD) chunk = UCV_UDP_FRAGMENT_PAYLOAD;
        if (offset + chunk >= payload.size()) flags |= UCV_FLAG_LAST_FRAGMENT;
      }

      ucv_frame_header_t h{};
      h.protocol_id   = UCV_PROTO_RAWUDP;
      h.flags         = flags;
      h.frame_seq     = seq;
      h.packet_seq    = packet_seq++;
      h.payload_bytes = static_cast<uint32_t>(payload.size());
      h.fragment_index = fragment_index++;
      h.fragment_count = fragment_count;
      h.t_capture_ns  = t_cap;
      h.t_encoded_ns  = t_enc;
      h.run_id_hash   = run_hash;
      h.t_sent_ns     = NowNs();   // stamped last, as every transport must
      ucv_frame_header_finalize(&h);

      std::memcpy(pkt.data(), &h, sizeof(h));
      std::memcpy(pkt.data() + UCV_FRAME_HEADER_SIZE, payload.data() + offset, chunk);
      sendto(v, reinterpret_cast<const char*>(pkt.data()),
             static_cast<int>(UCV_FRAME_HEADER_SIZE + chunk), 0,
             reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
      offset += chunk;
    } while (offset < payload.size());

    sent_frames++;
    seq++;
  }

  std::printf("mock: sent %llu frames, synthetically dropped %llu\n",
              static_cast<unsigned long long>(sent_frames),
              static_cast<unsigned long long>(dropped_frames));

  g_quit = true;
  control.join();
  CLOSESOCK(v);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
