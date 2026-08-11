// Minimal cross-platform networking for the receiver.
//
// Windows and POSIX only; kept deliberately small because the desktop app this
// feeds into will be C++ and should not inherit a heavyweight dependency for
// what amounts to a UDP socket and a clock.

#ifndef UCV_NET_H
#define UCV_NET_H

#include "ucv_wire.h"

#include <cstdint>
#include <string>

namespace ucv {

// Windows SOCKET is an unsigned 64-bit handle and INVALID_SOCKET is its
// all-ones value; storing it in a signed int (or truncating to int) can
// mangle a valid handle. POSIX fds are plain ints. Alias the real type per
// platform rather than assuming they are interchangeable.
#ifdef _WIN32
using SocketHandle = unsigned long long;
constexpr SocketHandle kInvalidSocket = ~0ull;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

bool NetInit();
void NetShutdown();

// Monotonic nanoseconds on the PC clock. Must be monotonic, not wall-clock:
// an NTP step mid-run would otherwise produce negative latencies.
uint64_t NowNs();

// Ctrl-C handling so a run always writes its log and summary rather than
// dying with the measurement half-recorded.
void InstallSignalHandler(void (*on_quit)());

std::string MakeRunId(const std::string& protocol);

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&)            = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool Bind(int port);
  bool SetRecvBuffer(int bytes);
  // Returns bytes received, 0 on timeout, -1 on error.
  int  RecvTimeout(void* buf, size_t len, int timeout_ms);
  void Close();

 private:
  SocketHandle fd_ = kInvalidSocket;
};

// Best-effort local preview output. The measurement receiver remains the sole
// listener on the phone's video port; complete H.264 frames are mirrored to a
// localhost dashboard socket so preview decoding cannot steal packets.
class UdpSender {
 public:
  ~UdpSender();
  bool Open(const std::string& peer_ip, int port);
  bool Send(const void* data, size_t len);
  void Close();

 private:
  SocketHandle fd_ = kInvalidSocket;
  uint32_t peer_addr_ = 0;
  int peer_port_ = 0;
};

class TcpClient {
 public:
  ~TcpClient();
  bool Connect(const std::string& peer_ip, int port, int timeout_ms);
  bool SendAll(const void* data, size_t len);
  int RecvTimeout(void* buf, size_t len, int timeout_ms);
  void Close();

 private:
  SocketHandle fd_ = kInvalidSocket;
};

// Result of the SNTP-style exchange in harness spec §4.1.
struct ClockSync {
  int64_t offset_ns   = 0;  // phone_clock - pc_clock
  int64_t best_rtt_ns = 0;  // lowest observed round trip
  int     samples     = 0;
};

class ControlClient {
 public:
  ~ControlClient();

  bool Open(const std::string& phone_ip, int port);

  // Runs `probes` PING exchanges and keeps the sample with the lowest RTT —
  // it has the least queueing noise and therefore the most trustworthy
  // offset. Returns false if nothing was answered.
  bool Synchronise(int probes, ClockSync* out);

  // Sends SET_ALPHA with ACK requested and measures the round trip entirely
  // on the PC clock, so this number does not inherit clock-sync error.
  bool SendSetAlphaAwaitAck(float alpha, int64_t* rtt_ns);

  // Website-centric lifecycle: receiver binds the video socket first, then
  // tells the phone exactly which advertised MJPEG mode to run.
  bool SetRunHash(const std::string& run_id);
  bool StartRemoteRun(int width, int height, int fps, uint8_t protocol_id,
                      int video_port);
  bool StopRemoteRun();

  void Close();

 private:
  bool Exchange(const ucv_control_t& cmd, ucv_ack_t* ack, int timeout_ms,
                uint64_t* t1_out, uint64_t* t4_out);

  SocketHandle fd_      = kInvalidSocket;
  uint32_t     cmd_seq_ = 0;
  std::string peer_ip_;
  int         peer_port_ = 0;
};

}  // namespace ucv

#endif  // UCV_NET_H
