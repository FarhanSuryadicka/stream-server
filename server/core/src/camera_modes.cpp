#include "ucv/camera_modes.hpp"

#include "ucv_wire.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ucv {

std::string CameraResolution::best_mode() const {
  if (fps.empty()) return {};
  return std::to_string(width) + "x" + std::to_string(height) + "@" +
         std::to_string(fps.front());
}

std::string CameraResolution::label() const {
  return std::to_string(width) + " x " + std::to_string(height);
}

namespace {

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

CameraModeQuery query_camera_modes(const std::string& phone_ip,
                                   int control_port, int timeout_ms) {
  CameraModeQuery result;

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    result.error = "cannot open control socket";
    return result;
  }
  DWORD tmo = static_cast<DWORD>(timeout_ms);
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
  const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    result.error = "cannot open control socket";
    return result;
  }
  timeval tmo{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
#endif

  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(static_cast<std::uint16_t>(control_port));
  if (::inet_pton(AF_INET, phone_ip.c_str(), &peer.sin_addr) != 1) {
#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    result.error = "invalid phone IP";
    return result;
  }

  // The agent answers one mode per index; walk until it stops applying.
  std::map<std::pair<int, int>, std::set<int>> parsed;
  std::uint32_t sequence =
      static_cast<std::uint32_t>(now_ns() / 1000000ull) & 0xFFFFFFFFu;

  for (int index = 0; index < 256; ++index) {
    sequence = (sequence + 1) & 0xFFFFFFFFu;

    ucv_control_t cmd{};
    cmd.version = 1;
    cmd.cmd_type = UCV_CMD_GET_MODE;
    cmd.flags = UCV_CTL_ACK_REQUESTED;
    cmd.cmd_seq = sequence;
    cmd.t_sent_ns = now_ns();
    const std::uint16_t idx16 = static_cast<std::uint16_t>(index);
    std::memcpy(cmd.payload, &idx16, sizeof(idx16));
    ucv_control_finalize(&cmd);

    bool answered = false;
    ucv_ack_t ack{};
    // Two attempts per index: a single dropped datagram on Wi-Fi would
    // otherwise truncate the mode list and hide resolutions the camera has.
    for (int attempt = 0; attempt < 2 && !answered; ++attempt) {
      ::sendto(sock, reinterpret_cast<const char*>(&cmd), sizeof(cmd), 0,
               reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
      char buffer[128];
#ifdef _WIN32
      const int n = ::recv(sock, buffer, sizeof(buffer), 0);
#else
      const ssize_t n = ::recv(sock, buffer, sizeof(buffer), 0);
#endif
      if (n != static_cast<int>(sizeof(ucv_ack_t))) continue;
      std::memcpy(&ack, buffer, sizeof(ack));
      if (!ucv_ack_valid(&ack)) continue;
      if (ack.cmd_type != UCV_CMD_GET_MODE || ack.cmd_seq != sequence) continue;
      answered = true;
    }

    if (!answered) {
      if (index == 0) {
        result.error =
            "camera agent tidak menjawab; buka kamera di aplikasi HP";
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return result;
      }
      break;   // ran past the last mode
    }
    if (!(ack.flags & UCV_ACK_APPLIED)) break;   // no more modes

    // The agent packs the mode into the echo timestamp fields, matching
    // query_camera_modes() in the Python dashboard.
    const int width = static_cast<int>((ack.t_recv_ns >> 32) & 0xFFFF);
    const int height = static_cast<int>(ack.t_recv_ns & 0xFFFF);
    const int fps = static_cast<int>((ack.t_ack_ns >> 32) & 0xFF);
    if (width > 0 && height > 0 && fps > 0)
      parsed[{width, height}].insert(fps);
  }

#ifdef _WIN32
  ::closesocket(sock);
#else
  ::close(sock);
#endif

  if (parsed.empty()) {
    result.error = "kamera terbuka tetapi tidak menawarkan mode MJPEG";
    return result;
  }

  // Smallest area first, so the conservative choice is at the top of the list
  // — high resolutions are the ones that stall over isochronous USB.
  std::vector<std::pair<std::pair<int, int>, std::set<int>>> ordered(
      parsed.begin(), parsed.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    return a.first.first * a.first.second < b.first.first * b.first.second;
  });

  for (const auto& [size, fps_set] : ordered) {
    CameraResolution r;
    r.width = size.first;
    r.height = size.second;
    r.fps.assign(fps_set.rbegin(), fps_set.rend());   // highest first
    result.resolutions.push_back(std::move(r));
  }
  result.ok = true;
  return result;
}

}  // namespace ucv
