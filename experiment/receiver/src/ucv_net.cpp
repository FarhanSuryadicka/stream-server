#include "ucv_net.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  using socklen_t = int;
  #define CLOSESOCK closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define CLOSESOCK close
#endif

namespace {
// select() takes nfds (highest fd + 1) on POSIX and ignores it on Windows.
// Wrapping it keeps the platform difference in one place instead of casting
// an unsigned Windows handle down to int at every call site.
inline int SelectNfds(ucv::SocketHandle fd) {
#ifdef _WIN32
  (void)fd;
  return 0;
#else
  return fd + 1;
#endif
}
}  // namespace

namespace ucv {

namespace {
void (*g_on_quit)() = nullptr;

void HandleSignal(int) {
  if (g_on_quit) g_on_quit();
}
}  // namespace

bool NetInit() {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

void NetShutdown() {
#ifdef _WIN32
  WSACleanup();
#endif
}

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void InstallSignalHandler(void (*on_quit)()) {
  g_on_quit = on_quit;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
}

std::string MakeRunId(const std::string& protocol) {
  const auto now = std::chrono::system_clock::now();
  const auto t   = std::chrono::system_clock::to_time_t(now);
  std::tm tmv{};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tmv);
  return std::string(stamp) + "-" + protocol;
}

// ---------------------------------------------------------------- UdpSocket

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Bind(int port) {
  fd_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (fd_ == kInvalidSocket) return false;

  int one = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&one), sizeof(one));

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(static_cast<uint16_t>(port));
  if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Close();
    return false;
  }
  return true;
}

bool UdpSocket::SetRecvBuffer(int bytes) {
  if (fd_ == kInvalidSocket) return false;
  return setsockopt(fd_, SOL_SOCKET, SO_RCVBUF,
                    reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

int UdpSocket::RecvTimeout(void* buf, size_t len, int timeout_ms) {
  if (fd_ == kInvalidSocket) return -1;

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd_, &rfds);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};

  const int r = select(SelectNfds(fd_), &rfds, nullptr, nullptr, &tv);
  if (r <= 0) return r == 0 ? 0 : -1;

  return recv(fd_, static_cast<char*>(buf), static_cast<int>(len), 0);
}

void UdpSocket::Close() {
  if (fd_ != kInvalidSocket) {
    CLOSESOCK(fd_);
    fd_ = kInvalidSocket;
  }
}

// --------------------------------------------------------------- UdpSender

UdpSender::~UdpSender() { Close(); }

bool UdpSender::Open(const std::string& peer_ip, int port) {
  Close();
  fd_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (fd_ == kInvalidSocket) return false;
  in_addr addr{};
  if (inet_pton(AF_INET, peer_ip.c_str(), &addr) != 1) {
    Close();
    return false;
  }
  peer_addr_ = addr.s_addr;
  peer_port_ = port;
  return true;
}

bool UdpSender::Send(const void* data, size_t len) {
  if (fd_ == kInvalidSocket || len > 65507) return false;
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = peer_addr_;
  peer.sin_port = htons(static_cast<uint16_t>(peer_port_));
  return sendto(fd_, static_cast<const char*>(data), static_cast<int>(len), 0,
                reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) ==
         static_cast<int>(len);
}

void UdpSender::Close() {
  if (fd_ != kInvalidSocket) {
    CLOSESOCK(fd_);
    fd_ = kInvalidSocket;
  }
}

// ------------------------------------------------------------ ControlClient

ControlClient::~ControlClient() { Close(); }

bool ControlClient::Open(const std::string& phone_ip, int port) {
  fd_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (fd_ == kInvalidSocket) return false;
  peer_ip_   = phone_ip;
  peer_port_ = port;
  return true;
}

bool ControlClient::Exchange(const ucv_control_t& cmd, ucv_ack_t* ack,
                             int timeout_ms, uint64_t* t1_out,
                             uint64_t* t4_out) {
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port   = htons(static_cast<uint16_t>(peer_port_));
  if (inet_pton(AF_INET, peer_ip_.c_str(), &peer.sin_addr) != 1) return false;

  // t1 is stamped immediately before the send syscall so that no local
  // preparation work is charged to the round trip.
  const uint64_t t1 = NowNs();
  ucv_control_t c = cmd;
  c.t_sent_ns = t1;
  ucv_control_finalize(&c);

  if (sendto(fd_, reinterpret_cast<const char*>(&c), sizeof(c), 0,
             reinterpret_cast<sockaddr*>(&peer),
             sizeof(peer)) != static_cast<int>(sizeof(c)))
    return false;

  // A late ACK from an earlier probe would otherwise be paired with this
  // probe's t1 and report an impossibly small RTT, so mismatched cmd_seq is
  // skipped rather than accepted.
  const uint64_t deadline = NowNs() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  for (;;) {
    const int64_t remain_ns = static_cast<int64_t>(deadline) - static_cast<int64_t>(NowNs());
    if (remain_ns <= 0) return false;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);
    timeval tv{static_cast<long>(remain_ns / 1000000000),
               static_cast<long>((remain_ns % 1000000000) / 1000)};
    if (select(SelectNfds(fd_), &rfds, nullptr, nullptr, &tv) <= 0)
      return false;

    ucv_ack_t a{};
    const int n = recv(fd_, reinterpret_cast<char*>(&a), sizeof(a), 0);
    const uint64_t t4 = NowNs();
    if (n != static_cast<int>(sizeof(a))) continue;
    if (!ucv_ack_valid(&a)) continue;
    if (a.cmd_seq != c.cmd_seq) continue;  // stale ACK from an earlier probe

    *ack    = a;
    *t1_out = t1;
    *t4_out = t4;
    return true;
  }
}

bool ControlClient::Synchronise(int probes, ClockSync* out) {
  int64_t best_rtt    = INT64_MAX;
  int64_t best_offset = 0;
  int     ok          = 0;

  for (int i = 0; i < probes; i++) {
    ucv_control_t cmd{};
    cmd.cmd_type = UCV_CMD_PING;
    cmd.flags    = UCV_CTL_ACK_REQUESTED;
    cmd.cmd_seq  = ++cmd_seq_;

    ucv_ack_t ack{};
    uint64_t t1 = 0, t4 = 0;
    if (!Exchange(cmd, &ack, 500, &t1, &t4)) continue;

    const int64_t rtt = ucv_round_trip_ns(t1, ack.t_recv_ns, ack.t_ack_ns, t4);
    if (rtt < 0) continue;  // impossible; discard rather than skew the estimate

    ok++;
    // Lowest RTT wins: least queueing noise, so the symmetry assumption the
    // offset formula relies on is least violated (harness spec §4.1).
    if (rtt < best_rtt) {
      best_rtt    = rtt;
      best_offset = ucv_clock_offset_ns(t1, ack.t_recv_ns, ack.t_ack_ns, t4);
    }
  }

  if (ok == 0) return false;
  out->offset_ns   = best_offset;
  out->best_rtt_ns = best_rtt;
  out->samples     = ok;
  return true;
}

bool ControlClient::SendSetAlphaAwaitAck(float alpha, int64_t* rtt_ns) {
  ucv_control_t cmd{};
  cmd.cmd_type = UCV_CMD_SET_ALPHA;
  cmd.flags    = UCV_CTL_ACK_REQUESTED;
  cmd.cmd_seq  = ++cmd_seq_;
  std::memcpy(cmd.payload, &alpha, sizeof(alpha));

  ucv_ack_t ack{};
  uint64_t t1 = 0, t4 = 0;
  if (!Exchange(cmd, &ack, 500, &t1, &t4)) return false;

  // Pure PC-clock round trip — no clock-sync error inherited. This is why
  // the harness treats control RTT as its most trustworthy number.
  *rtt_ns = static_cast<int64_t>(t4) - static_cast<int64_t>(t1);
  return true;
}

bool ControlClient::SetRunHash(const std::string& run_id) {
  ucv_control_t cmd{};
  cmd.cmd_type = UCV_CMD_SET_RUN_HASH;
  cmd.flags = UCV_CTL_ACK_REQUESTED;
  cmd.cmd_seq = ++cmd_seq_;
  const uint32_t hash = ucv_run_id_hash(run_id.c_str());
  std::memcpy(cmd.payload, &hash, sizeof(hash));
  ucv_ack_t ack{};
  uint64_t t1 = 0, t4 = 0;
  return Exchange(cmd, &ack, 1000, &t1, &t4) &&
         (ack.flags & UCV_ACK_APPLIED);
}

bool ControlClient::StartRemoteRun(int width, int height, int fps,
                                   uint8_t protocol_id, int video_port) {
  if (width <= 0 || width > 65535 || height <= 0 || height > 65535 ||
      fps <= 0 || fps > 255 || video_port <= 0 || video_port > 65535)
    return false;
  ucv_control_t cmd{};
  cmd.cmd_type = UCV_CMD_START_RUN;
  cmd.flags = UCV_CTL_ACK_REQUESTED;
  cmd.cmd_seq = ++cmd_seq_;
  ucv_start_run_payload_t request{
      static_cast<uint16_t>(width), static_cast<uint16_t>(height),
      static_cast<uint8_t>(fps), protocol_id,
      static_cast<uint16_t>(video_port)};
  std::memcpy(cmd.payload, &request, sizeof(request));
  ucv_ack_t ack{};
  uint64_t t1 = 0, t4 = 0;
  return Exchange(cmd, &ack, 5000, &t1, &t4) &&
         (ack.flags & UCV_ACK_APPLIED);
}

bool ControlClient::StopRemoteRun() {
  ucv_control_t cmd{};
  cmd.cmd_type = UCV_CMD_STOP_RUN;
  cmd.flags = UCV_CTL_ACK_REQUESTED;
  cmd.cmd_seq = ++cmd_seq_;
  ucv_ack_t ack{};
  uint64_t t1 = 0, t4 = 0;
  return Exchange(cmd, &ack, 5000, &t1, &t4) &&
         (ack.flags & UCV_ACK_APPLIED);
}

void ControlClient::Close() {
  if (fd_ != kInvalidSocket) {
    CLOSESOCK(fd_);
    fd_ = kInvalidSocket;
  }
}

}  // namespace ucv
