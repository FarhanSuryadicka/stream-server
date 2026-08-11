// ucv-receiver — PC-side measurement receiver for the protocol experiment.
//
// Responsibilities, in the order they matter:
//   1. Synchronise clocks with the phone (harness spec §4). Without this,
//      every one-way latency number below is fiction.
//   2. Receive the video stream over the protocol under test.
//   3. Log RAW timestamps to NDJSON — never derived values, so a corrected
//      metric formula can be re-applied to old runs without re-measuring.
//   4. Drive the control channel and measure its round-trip under load.
//
// Deliberately NOT done here: decoding and display. Decode cost is measured
// separately (harness spec §5.3) and mixing a display pipeline into the
// receiver would put GPU scheduling jitter inside the transport measurement.
//
// Build: see CMakeLists.txt in this directory.

#include "ucv_wire.h"
#include "ucv_stats.h"
#include "ucv_net.h"
#include "ucv_log.h"
#include <srt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};

struct Options {
  std::string protocol   = "raw_udp";
  std::string phone_ip;
  int         video_port   = UCV_PORT_RAWUDP;
  int         control_port = UCV_PORT_CONTROL;
  int         duration_s   = 120;
  int         warmup_s     = 10;
  std::string run_id;
  std::string out_dir = ".";
  std::string mode = "unknown";
  std::string condition = "unspecified";
  bool        control_load = true;  // exercise control channel during video
  int         control_hz   = 10;
  int         preview_port = 0;  // localhost UDP; 0 disables preview
  bool        remote_phone = true;
};

void PrintUsage(const char* argv0) {
  std::printf(
      "ucv-receiver — protocol experiment measurement receiver\n"
      "\n"
      "Usage: %s --phone <ip> [options]\n"
      "\n"
      "  --phone <ip>          Phone IP address (required)\n"
      "  --protocol <name>     raw_udp | rtp_udp | rtsp | srt | mjpeg\n"
      "                        (default: raw_udp. rtsp receives the same RTP\n"
      "                         packets as rtp_udp; only the sender differs,\n"
      "                         so the run records which one was used.)\n"
      "  --video-port <n>      Video port      (default: protocol default)\n"
      "  --control-port <n>    Control port    (default: %d)\n"
      "  --duration <s>        Run length      (default: 120)\n"
      "  --warmup <s>          Discarded head  (default: 10)\n"
      "  --run-id <id>         Run identifier  (default: auto)\n"
      "  --out <dir>           Log directory   (default: .)\n"
      "  --mode <WxH@fps>      Capture mode metadata (default: unknown)\n"
      "  --condition <name>    Network condition label (default: unspecified)\n"
      "  --preview-port <n>    Mirror frames to localhost dashboard\n"
      "  --manual-phone        Do not remotely start/stop the phone pipeline\n"
      "  --no-control-load     Do not send control commands during the run\n"
      "  --control-hz <n>      Control command rate (default: 10)\n"
      "\n"
      "Control RTT is measured WHILE video streams, because measuring it on\n"
      "an idle link produces a best case that does not exist in deployment.\n",
      argv0, UCV_PORT_CONTROL);
}

bool ParseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return false; }
    else if (a == "--phone")        { auto v = next("--phone");        if (!v) return false; o->phone_ip = v; }
    else if (a == "--protocol")     { auto v = next("--protocol");     if (!v) return false; o->protocol = v; }
    else if (a == "--video-port")   { auto v = next("--video-port");   if (!v) return false; o->video_port = std::atoi(v); }
    else if (a == "--control-port") { auto v = next("--control-port"); if (!v) return false; o->control_port = std::atoi(v); }
    else if (a == "--duration")     { auto v = next("--duration");     if (!v) return false; o->duration_s = std::atoi(v); }
    else if (a == "--warmup")       { auto v = next("--warmup");       if (!v) return false; o->warmup_s = std::atoi(v); }
    else if (a == "--run-id")       { auto v = next("--run-id");       if (!v) return false; o->run_id = v; }
    else if (a == "--out")          { auto v = next("--out");          if (!v) return false; o->out_dir = v; }
    else if (a == "--mode")         { auto v = next("--mode");         if (!v) return false; o->mode = v; }
    else if (a == "--condition")    { auto v = next("--condition");    if (!v) return false; o->condition = v; }
    else if (a == "--preview-port") { auto v = next("--preview-port"); if (!v) return false; o->preview_port = std::atoi(v); }
    else if (a == "--control-hz")   { auto v = next("--control-hz");   if (!v) return false; o->control_hz = std::atoi(v); }
    else if (a == "--no-control-load") { o->control_load = false; }
    else if (a == "--manual-phone") { o->remote_phone = false; }
    else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      return false;
    }
  }
  if (o->phone_ip.empty()) {
    std::fprintf(stderr, "error: --phone is required\n\n");
    PrintUsage(argv[0]);
    return false;
  }
  if (o->run_id.empty()) o->run_id = ucv::MakeRunId(o->protocol);
  if (o->protocol == "mjpeg" && o->video_port == UCV_PORT_RAWUDP)
    o->video_port = UCV_PORT_MJPEG;
  if (o->protocol == "rtp_udp" && o->video_port == UCV_PORT_RAWUDP)
    o->video_port = UCV_PORT_RTP;
  if (o->protocol == "srt" && o->video_port == UCV_PORT_RAWUDP)
    o->video_port = UCV_PORT_SRT;
  // RTSP signalling carries the very same RTP packets, so the media still
  // arrives on the RTP port and is parsed by the RTP path below.
  if (o->protocol == "rtsp" && o->video_port == UCV_PORT_RAWUDP)
    o->video_port = UCV_PORT_RTP;
  if (o->preview_port < 0 || o->preview_port > 65535) {
    std::fprintf(stderr, "error: invalid --preview-port\n");
    return false;
  }
  return true;
}

#pragma pack(push, 1)
struct PreviewHeader {
  uint32_t magic;
  uint32_t frame_seq;
  uint32_t frame_bytes;
  uint16_t fragment_index;
  uint16_t fragment_count;
};
#pragma pack(pop)

constexpr uint32_t kPreviewMagic = 0x31565055;  // "UPV1" in little endian
constexpr uint32_t kPreviewJpegMagic = 0x314A5055;  // "UPJ1"
constexpr size_t kPreviewChunk = 60000;

// ---------------------------------------------------------------------
// Frame reassembly for the UDP family (wire spec §1.1)
// ---------------------------------------------------------------------
// Fragment index is implicit — reassembly relies on arrival order within a
// frame. A gap therefore invalidates the frame, and that is counted as a
// reassembly failure rather than as network loss, so the two never get
// conflated in the results.
class Reassembler {
 public:
  struct Complete {
    uint32_t seq;
    uint64_t t_capture_ns;
    uint64_t t_encoded_ns;
    uint64_t t_sent_ns;   // from the LAST fragment: closest to the wire
    uint64_t t_recv_ns;   // when the last fragment landed
    uint32_t bytes;
    bool     keyframe;
    uint32_t run_hash = 0;
    uint32_t wire_bytes = 0;
    uint64_t tcp_segments = 0;
    uint64_t tcp_retrans = 0;
    bool     tcp_stats = false;
    std::vector<uint8_t> payload;
  };

  // Returns true and fills `out` when a frame completes.
  bool Push(const ucv_frame_header_t& h, const uint8_t* chunk,
            size_t chunk_bytes,
            uint64_t t_recv_ns, Complete* out) {
    if (!(h.flags & UCV_FLAG_FRAGMENTED)) {
      if (chunk_bytes < h.payload_bytes) {
        failures_++;
        return false;
      }
      out->seq          = h.frame_seq;
      out->t_capture_ns = h.t_capture_ns;
      out->t_encoded_ns = h.t_encoded_ns;
      out->t_sent_ns    = h.t_sent_ns;
      out->t_recv_ns    = t_recv_ns;
      out->bytes        = h.payload_bytes;
      out->keyframe     = (h.flags & UCV_FLAG_KEYFRAME) != 0;
      out->payload.assign(chunk, chunk + h.payload_bytes);
      return true;
    }

    // A new frame_seq arriving while another is open means the open one
    // lost its tail — count it and drop it.
    if (open_ && open_seq_ != h.frame_seq) {
      failures_++;
      open_ = false;
    }
    if (!open_) {
      open_        = true;
      open_seq_    = h.frame_seq;
      accumulated_ = 0;
      payload_.clear();
      payload_.reserve(h.payload_bytes);
    }
    accumulated_ += chunk_bytes;
    payload_.insert(payload_.end(), chunk, chunk + chunk_bytes);

    if (h.flags & UCV_FLAG_LAST_FRAGMENT) {
      open_ = false;
      // Short reassembly = a middle fragment was lost.
      if (accumulated_ < h.payload_bytes) {
        failures_++;
        return false;
      }
      out->seq          = h.frame_seq;
      out->t_capture_ns = h.t_capture_ns;
      out->t_encoded_ns = h.t_encoded_ns;
      out->t_sent_ns    = h.t_sent_ns;
      out->t_recv_ns    = t_recv_ns;
      out->bytes        = h.payload_bytes;
      out->keyframe     = (h.flags & UCV_FLAG_KEYFRAME) != 0;
      out->payload = std::move(payload_);
      if (out->payload.size() > h.payload_bytes)
        out->payload.resize(h.payload_bytes);
      return true;
    }
    return false;
  }

  uint64_t failures() const { return failures_; }

 private:
  bool     open_        = false;
  uint32_t open_seq_    = 0;
  uint64_t accumulated_ = 0;
  uint64_t failures_    = 0;
  std::vector<uint8_t> payload_;
};

// RFC 6184 RTP/H.264 reassembly. Measurement metadata is carried in an
// RFC 8285 two-byte extension (profile 0x1000, element id 1). Fragments are
// stored by explicit index so ordinary UDP reordering does not corrupt video.
class RtpReassembler {
 public:
  int Push(const uint8_t* packet, size_t packet_bytes, uint64_t t_recv_ns,
           uint32_t expected_run_hash, ucv_frame_header_t* header,
           Reassembler::Complete* out) {
    if (packet_bytes < 12 || (packet[0] >> 6) != 2 || !(packet[0] & 0x10))
      return -1;
    const size_t csrc_bytes = static_cast<size_t>(packet[0] & 0x0f) * 4;
    size_t pos = 12 + csrc_bytes;
    if (pos + 4 > packet_bytes) return -1;
    const uint16_t profile = ReadBe16(packet + pos);
    const size_t ext_bytes = static_cast<size_t>(ReadBe16(packet + pos + 2)) * 4;
    pos += 4;
    if (profile != 0x1000 || pos + ext_bytes > packet_bytes) return -1;

    bool found = false;
    size_t ext = pos, ext_end = pos + ext_bytes;
    while (ext + 2 <= ext_end) {
      const uint8_t id = packet[ext++];
      const uint8_t len = packet[ext++];
      if (id == 0) continue;
      if (ext + len > ext_end) return -1;
      if (id == 1 && len == UCV_FRAME_HEADER_SIZE) {
        std::memcpy(header, packet + ext, sizeof(*header));
        found = true;
        break;
      }
      ext += len;
    }
    pos = ext_end;
    if (!found || !ucv_frame_header_valid(header) ||
        header->protocol_id != UCV_PROTO_RTSP ||
        !header->fragment_count ||
        header->fragment_count > 4096 ||
        header->fragment_index >= header->fragment_count || pos >= packet_bytes)
      return -1;
    if (header->run_id_hash != expected_run_hash) return -2;

    if (open_ && open_seq_ != header->frame_seq) {
      failures_++;
      Reset();
    }
    if (!open_) {
      open_ = true;
      open_seq_ = header->frame_seq;
      expected_ = header->fragment_count;
      chunks_.assign(expected_, {});
      present_.assign(expected_, false);
      received_ = 0;
      first_ = *header;
      last_sent_ns_ = header->t_sent_ns;
    }
    if (expected_ != header->fragment_count) {
      failures_++;
      Reset();
      return -1;
    }

    const uint8_t* payload = packet + pos;
    const size_t payload_bytes = packet_bytes - pos;
    std::vector<uint8_t> decoded;
    const uint8_t nal_type = payload[0] & 0x1f;
    if (nal_type >= 1 && nal_type <= 23) {
      static const uint8_t start_code[] = {0, 0, 0, 1};
      decoded.insert(decoded.end(), start_code, start_code + 4);
      decoded.insert(decoded.end(), payload, payload + payload_bytes);
    } else if (nal_type == 28 && payload_bytes >= 3) {
      const uint8_t fu = payload[1];
      if (fu & 0x80) {
        static const uint8_t start_code[] = {0, 0, 0, 1};
        decoded.insert(decoded.end(), start_code, start_code + 4);
        decoded.push_back(static_cast<uint8_t>((payload[0] & 0xe0) | (fu & 0x1f)));
      }
      decoded.insert(decoded.end(), payload + 2, payload + payload_bytes);
    } else {
      failures_++;
      Reset();
      return -1;
    }

    const uint16_t index = header->fragment_index;
    if (!present_[index]) {
      chunks_[index] = std::move(decoded);
      present_[index] = true;
      received_++;
    }
    if (index + 1 == expected_) last_sent_ns_ = header->t_sent_ns;
    if (received_ != expected_) return 0;

    out->seq = first_.frame_seq;
    out->t_capture_ns = first_.t_capture_ns;
    out->t_encoded_ns = first_.t_encoded_ns;
    out->t_sent_ns = last_sent_ns_;
    out->t_recv_ns = t_recv_ns;
    out->bytes = first_.payload_bytes;
    out->keyframe = (first_.flags & UCV_FLAG_KEYFRAME) != 0;
    out->run_hash = first_.run_id_hash;
    out->payload.clear();
    for (auto& chunk : chunks_)
      out->payload.insert(out->payload.end(), chunk.begin(), chunk.end());
    Reset();
    return 1;
  }

  uint64_t failures() const { return failures_; }

 private:
  static uint16_t ReadBe16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
  }
  void Reset() {
    open_ = false; expected_ = received_ = 0;
    chunks_.clear(); present_.clear();
  }
  bool open_ = false;
  uint32_t open_seq_ = 0;
  uint16_t expected_ = 0, received_ = 0;
  uint64_t last_sent_ns_ = 0, failures_ = 0;
  ucv_frame_header_t first_{};
  std::vector<std::vector<uint8_t>> chunks_;
  std::vector<bool> present_;
};

// Packet-level accounting for Raw UDP. Unlike frame_seq, packet_seq advances
// for every datagram, so loss of one fragment is visible even when the whole
// video frame consequently cannot be decoded. Missing() is derived from the
// unique set and therefore heals when a reordered packet arrives later.
class PacketTracker {
 public:
  bool Observe(uint32_t packet_seq) {
    if (!seen_.insert(packet_seq).second) {
      duplicates_++;
      return false;
    }
    if (!have_) {
      have_ = true;
      min_ = max_ = packet_seq;
    } else {
      if (packet_seq < max_) reorder_events_++;
      if (packet_seq < min_) min_ = packet_seq;
      if (packet_seq > max_) max_ = packet_seq;
    }
    return true;
  }
  uint64_t received() const { return seen_.size(); }
  uint64_t missing() const {
    if (!have_) return 0;
    return static_cast<uint64_t>(max_ - min_) + 1 - seen_.size();
  }
  uint64_t reorder_events() const { return reorder_events_; }
  uint64_t duplicates() const { return duplicates_; }
  double loss_pct() const {
    const uint64_t total = received() + missing();
    return total ? 100.0 * static_cast<double>(missing()) / total : 0.0;
  }

 private:
  bool have_ = false;
  uint32_t min_ = 0, max_ = 0;
  uint64_t reorder_events_ = 0, duplicates_ = 0;
  std::unordered_set<uint32_t> seen_;
};

void SendPreviewFrame(ucv::UdpSender* sender, const Reassembler::Complete& frame,
                      bool jpeg) {
  if (!sender || frame.payload.empty()) return;
  const size_t count = (frame.payload.size() + kPreviewChunk - 1) / kPreviewChunk;
  if (count == 0 || count > 65535) return;
  std::vector<uint8_t> packet(sizeof(PreviewHeader) + kPreviewChunk);
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = i * kPreviewChunk;
    const size_t bytes = std::min(kPreviewChunk, frame.payload.size() - offset);
    PreviewHeader h{jpeg ? kPreviewJpegMagic : kPreviewMagic, frame.seq,
                    static_cast<uint32_t>(frame.payload.size()),
                    static_cast<uint16_t>(i), static_cast<uint16_t>(count)};
    std::memcpy(packet.data(), &h, sizeof(h));
    std::memcpy(packet.data() + sizeof(h), frame.payload.data() + offset, bytes);
    if (!sender->Send(packet.data(), sizeof(h) + bytes)) break;
  }
}

class MjpegReader {
 public:
  bool Connect(const std::string& phone, int port) {
    if (!tcp_.Connect(phone, port, 1000)) return false;
    static const char request[] =
        "GET / HTTP/1.1\r\nHost: phone\r\nConnection: close\r\n\r\n";
    return tcp_.SendAll(request, sizeof(request) - 1);
  }

  int ReadFrame(Reassembler::Complete* out, int timeout_ms) {
    for (;;) {
      if (!response_done_) {
        const size_t end = buffer_.find("\r\n\r\n");
        if (end != std::string::npos) {
          if (buffer_.compare(0, 12, "HTTP/1.1 200") != 0) return -1;
          buffer_.erase(0, end + 4);
          response_done_ = true;
        }
      } else {
        const size_t boundary = buffer_.find("--frame\r\n");
        if (boundary != std::string::npos) {
          if (boundary) buffer_.erase(0, boundary);
          const size_t headers_end = buffer_.find("\r\n\r\n", 9);
          if (headers_end != std::string::npos) {
            const std::string headers = buffer_.substr(9, headers_end - 9);
            uint64_t length = 0, seq = 0, cap = 0, enc = 0, snd = 0;
            uint64_t key = 0, run = 0, net_stats = 0;
            uint64_t tcp_segments = 0, tcp_retrans = 0;
            if (!HeaderNumber(headers, "Content-Length", &length) ||
                !HeaderNumber(headers, "X-UCV-Seq", &seq) ||
                !HeaderNumber(headers, "X-UCV-Cap-Ns", &cap) ||
                !HeaderNumber(headers, "X-UCV-Enc-Ns", &enc) ||
                !HeaderNumber(headers, "X-UCV-Snd-Ns", &snd) ||
                !HeaderNumber(headers, "X-UCV-Key", &key) ||
                !HeaderNumber(headers, "X-UCV-Run", &run) ||
                length == 0 || length > 32 * 1024 * 1024) return -1;
            const bool have_tcp_stats =
                HeaderNumber(headers, "X-UCV-Net-Stats", &net_stats) &&
                HeaderNumber(headers, "X-UCV-TCP-Segments", &tcp_segments) &&
                HeaderNumber(headers, "X-UCV-TCP-Retrans", &tcp_retrans) &&
                net_stats != 0;
            const size_t body = headers_end + 4;
            const size_t total = body + static_cast<size_t>(length) + 2;
            if (buffer_.size() >= total) {
              out->seq = static_cast<uint32_t>(seq);
              out->t_capture_ns = cap;
              out->t_encoded_ns = enc;
              out->t_sent_ns = snd;
              out->t_recv_ns = ucv::NowNs();
              out->bytes = static_cast<uint32_t>(length);
              out->keyframe = key != 0;
              out->run_hash = static_cast<uint32_t>(run);
              out->wire_bytes = static_cast<uint32_t>(total);
              out->tcp_segments = tcp_segments;
              out->tcp_retrans = tcp_retrans;
              out->tcp_stats = have_tcp_stats;
              out->payload.assign(buffer_.begin() + body,
                                  buffer_.begin() + body + length);
              buffer_.erase(0, total);
              return 1;
            }
          }
        } else if (buffer_.size() > 1024) {
          buffer_.erase(0, buffer_.size() - 8);
        }
      }
      char chunk[64 * 1024];
      const int n = tcp_.RecvTimeout(chunk, sizeof(chunk), timeout_ms);
      if (n <= 0) return n;
      buffer_.append(chunk, static_cast<size_t>(n));
    }
  }

 private:
  static bool HeaderNumber(const std::string& headers, const char* name,
                           uint64_t* value) {
    const std::string needle = std::string(name) + ":";
    size_t pos = headers.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < headers.size() && headers[pos] == ' ') pos++;
    const size_t end = headers.find("\r\n", pos);
    try {
      *value = std::stoull(headers.substr(pos, end - pos));
      return true;
    } catch (...) {
      return false;
    }
  }

  ucv::TcpClient tcp_;
  std::string buffer_;
  bool response_done_ = false;
};

class SrtReceiver {
 public:
  ~SrtReceiver() { Close(); }

  bool Listen(int port) {
    if (srt_startup() == SRT_ERROR) return false;
    listener_ = srt_create_socket();
    if (listener_ == SRT_INVALID_SOCK) return false;
    SRT_TRANSTYPE type = SRTT_LIVE;
    int yes = 1, no = 0, latency = 20;
    int payload = UCV_FRAME_HEADER_SIZE + UCV_UDP_FRAGMENT_PAYLOAD;
    if (!Set(listener_, SRTO_TRANSTYPE, &type, sizeof(type)) ||
        !Set(listener_, SRTO_MESSAGEAPI, &yes, sizeof(yes)) ||
        !Set(listener_, SRTO_TLPKTDROP, &yes, sizeof(yes)) ||
        !Set(listener_, SRTO_LATENCY, &latency, sizeof(latency)) ||
        !Set(listener_, SRTO_PAYLOADSIZE, &payload, sizeof(payload)) ||
        !Set(listener_, SRTO_RCVSYN, &no, sizeof(no))) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    return srt_bind(listener_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) != SRT_ERROR &&
           srt_listen(listener_, 1) != SRT_ERROR;
  }

  bool Accept(int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
      sockaddr_storage peer{};
      int peer_size = sizeof(peer);
      socket_ = srt_accept(listener_, reinterpret_cast<sockaddr*>(&peer),
                           &peer_size);
      if (socket_ != SRT_INVALID_SOCK) {
        int timeout = 200;
        Set(socket_, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  int Recv(void* data, int bytes) {
    const int result = srt_recvmsg(socket_, static_cast<char*>(data), bytes);
    if (result != SRT_ERROR) return result;
    const int code = srt_getlasterror(nullptr);
    return code == SRT_ETIMEOUT || code == SRT_EASYNCRCV ? 0 : -1;
  }

  bool Stats(uint64_t* received, uint64_t* lost, uint64_t* retransmitted) {
    SRT_TRACEBSTATS stats{};
    if (socket_ == SRT_INVALID_SOCK || srt_bstats(socket_, &stats, 0) == SRT_ERROR)
      return false;
    *received = static_cast<uint64_t>(std::max<int64_t>(0, stats.pktRecvTotal));
    *lost = static_cast<uint64_t>(std::max(0, stats.pktRcvLossTotal));
    *retransmitted = static_cast<uint64_t>(
        std::max(0, stats.pktRcvRetrans));
    return true;
  }

  void Close() {
    if (socket_ != SRT_INVALID_SOCK) srt_close(socket_);
    if (listener_ != SRT_INVALID_SOCK) srt_close(listener_);
    socket_ = listener_ = SRT_INVALID_SOCK;
    srt_cleanup();
  }

 private:
  static bool Set(SRTSOCKET socket, SRT_SOCKOPT option,
                  const void* value, int size) {
    return srt_setsockflag(socket, option, value, size) != SRT_ERROR;
  }
  SRTSOCKET listener_ = SRT_INVALID_SOCK;
  SRTSOCKET socket_ = SRT_INVALID_SOCK;
};

}  // namespace

int main(int argc, char** argv) {
  // The dashboard captures stdout through a pipe. Disable stdio buffering so
  // clock-sync and five-second progress lines are visible immediately.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 1;

  ucv::InstallSignalHandler([] { g_quit = true; });
  if (!ucv::NetInit()) {
    std::fprintf(stderr, "fatal: network init failed\n");
    return 1;
  }

  std::printf("run_id   : %s\n", opt.run_id.c_str());
  std::printf("protocol : %s\n", opt.protocol.c_str());
  std::printf("phone    : %s (video :%d, control :%d)\n",
              opt.phone_ip.c_str(), opt.video_port, opt.control_port);
  std::printf("duration : %d s (%d s warmup discarded)\n\n",
              opt.duration_s, opt.warmup_s);
  std::printf("mode     : %s\n", opt.mode.c_str());
  std::printf("condition: %s\n\n", opt.condition.c_str());

  // ------------------------------------------------------------------
  // 1. Clock synchronisation BEFORE the run (harness spec §4)
  // ------------------------------------------------------------------
  ucv::ControlClient control;
  if (!control.Open(opt.phone_ip, opt.control_port)) {
    std::fprintf(stderr, "fatal: cannot open control socket to %s:%d\n",
                 opt.phone_ip.c_str(), opt.control_port);
    return 1;
  }

  std::printf("[clock] synchronising (64 probes)...\n");
  ucv::ClockSync sync_before;
  if (!control.Synchronise(64, &sync_before)) {
    std::fprintf(stderr,
                 "fatal: clock sync failed — is the phone's control channel\n"
                 "       running? Every latency number depends on this, so\n"
                 "       the run is aborted rather than reported as valid.\n");
    return 1;
  }
  std::printf("[clock] offset = %.3f ms, best RTT = %.3f ms\n",
              sync_before.offset_ns / 1e6, sync_before.best_rtt_ns / 1e6);

  // ------------------------------------------------------------------
  // 2. Open the video receiver
  // ------------------------------------------------------------------
  const std::string log_path = opt.out_dir + "/receiver-" + opt.run_id + ".ndjson";
  if (std::filesystem::exists(log_path)) {
    std::fprintf(stderr,
                 "fatal: refusing to overwrite existing run log '%s'; use a new --run-id\n",
                 log_path.c_str());
    return 1;
  }
  ucv::NdjsonWriter log(log_path);
  if (!log.ok()) {
    std::fprintf(stderr, "fatal: cannot write log in '%s'\n", opt.out_dir.c_str());
    return 1;
  }
  log.WriteMeta(opt.run_id, opt.protocol, opt.mode, opt.condition,
                sync_before.offset_ns,
                sync_before.best_rtt_ns);

  // RTSP signalling carries plain RTP packets, so every RTP-shaped code path
  // below must accept it too. One predicate rather than a string comparison
  // repeated at each site, so a missed site cannot silently mis-parse.
  const bool rtp_wire = opt.protocol == "rtp_udp" || opt.protocol == "rtsp";

  ucv::UdpSocket video;
  ucv::UdpSender preview;
  MjpegReader mjpeg;
  SrtReceiver srt;
  if (opt.protocol == "raw_udp" || opt.protocol == "rtp_udp" ||
      opt.protocol == "rtsp") {
    if (!video.Bind(opt.video_port)) {
      std::fprintf(stderr, "fatal: cannot bind UDP :%d\n", opt.video_port);
      return 1;
    }
    // Large receive buffer: a keyframe burst dropped in the kernel would be
    // miscounted as network loss and would corrupt the reliability metric.
    video.SetRecvBuffer(8 * 1024 * 1024);
    if (opt.preview_port && !preview.Open("127.0.0.1", opt.preview_port)) {
      std::fprintf(stderr, "warning: cannot open local preview output :%d\n",
                   opt.preview_port);
      opt.preview_port = 0;
    }
  } else if (opt.protocol == "mjpeg") {
    if (opt.preview_port && !preview.Open("127.0.0.1", opt.preview_port)) {
      std::fprintf(stderr, "warning: cannot open local preview output :%d\n",
                   opt.preview_port);
      opt.preview_port = 0;
    }
  } else if (opt.protocol == "srt") {
    if (!srt.Listen(opt.video_port)) {
      std::fprintf(stderr, "fatal: cannot listen for SRT on :%d: %s\n",
                   opt.video_port, srt_getlasterror_str());
      return 1;
    }
    if (opt.preview_port && !preview.Open("127.0.0.1", opt.preview_port)) {
      std::fprintf(stderr, "warning: cannot open local preview output :%d\n",
                   opt.preview_port);
      opt.preview_port = 0;
    }
  } else if (false) {
    std::fprintf(stderr,
                 "error: the mjpeg receiver is a separate reader (HTTP\n"
                 "       multipart, not datagrams). Not yet implemented in\n"
                 "       this binary — see docs/03-implementation-plan.md.\n");
    return 2;
  } else {
    std::fprintf(stderr, "error: protocol '%s' not implemented in this receiver\n",
                 opt.protocol.c_str());
    return 2;
  }

  if (opt.remote_phone) {
    int mode_w = 0, mode_h = 0, mode_fps = 0;
    if (std::sscanf(opt.mode.c_str(), "%dx%d@%d", &mode_w, &mode_h,
                    &mode_fps) != 3) {
      std::fprintf(stderr,
                   "fatal: remote phone control needs --mode WxH@fps\n");
      return 2;
    }
    const uint8_t protocol_id = opt.protocol == "mjpeg" ? UCV_PROTO_MJPEG :
        (rtp_wire ? UCV_PROTO_RTSP :
         (opt.protocol == "srt" ? UCV_PROTO_SRT : UCV_PROTO_RAWUDP));
    if (!control.SetRunHash(opt.run_id) ||
        !control.StartRemoteRun(mode_w, mode_h, mode_fps,
                                protocol_id, opt.video_port)) {
      std::fprintf(stderr,
                   "fatal: phone rejected remote START for %s; refresh camera "
                   "modes and check the phone log\n",
                   opt.mode.c_str());
      return 2;
    }
    std::printf("[phone] pipeline started remotely: %s -> this PC:%d\n",
                opt.mode.c_str(), opt.video_port);
  }

  if (opt.protocol == "mjpeg") {
    bool connected = false;
    for (int attempt = 0; attempt < 30 && !connected; ++attempt) {
      connected = mjpeg.Connect(opt.phone_ip, opt.video_port);
      if (!connected)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
      std::fprintf(stderr, "fatal: cannot connect to MJPEG HTTP %s:%d\n",
                   opt.phone_ip.c_str(), opt.video_port);
      if (opt.remote_phone) control.StopRemoteRun();
      return 2;
    }
    std::printf("[recv] connected to MJPEG HTTP stream\n");
  } else if (opt.protocol == "srt") {
    if (!srt.Accept(5000)) {
      std::fprintf(stderr, "fatal: phone did not connect to SRT listener: %s\n",
                   srt_getlasterror_str());
      if (opt.remote_phone) control.StopRemoteRun();
      return 2;
    }
    std::printf("[recv] SRT connected (live/message, latency=20ms, encryption=off)\n");
  }

  // ------------------------------------------------------------------
  // 3. Control load thread — RTT measured WHILE video flows
  // ------------------------------------------------------------------
  ucv::Distribution control_rtt;
  std::atomic<uint64_t> control_sent{0}, control_acked{0};
  std::thread control_thread;
  if (opt.control_load) {
    control_thread = std::thread([&] {
      const auto period = std::chrono::milliseconds(
          opt.control_hz > 0 ? 1000 / opt.control_hz : 100);
      float angle = 0.0f;
      while (!g_quit) {
        int64_t rtt_ns = 0;
        angle += 0.05f;
        if (angle > 6.28f) angle = 0.0f;
        control_sent++;
        if (control.SendSetAlphaAwaitAck(angle, &rtt_ns)) {
          control_acked++;
          control_rtt.Add(rtt_ns / 1e6);
        }
        std::this_thread::sleep_for(period);
      }
    });
  }

  // ------------------------------------------------------------------
  // 4. Receive loop
  // ------------------------------------------------------------------
  ucv::SequenceTracker seq;
  PacketTracker        packets;
  ucv::JitterEstimator jitter;
  ucv::Distribution    transport_ms, glass_ms, encode_ms;
  Reassembler          reasm;
  RtpReassembler       rtp_reasm;

  uint64_t bytes_payload = 0, bytes_wire = 0;
  uint64_t bad_header = 0, wrong_run = 0;
  uint64_t tcp_segments = 0, tcp_retrans = 0;
  uint64_t tcp_base_segments = 0, tcp_base_retrans = 0;
  bool tcp_baseline = false, tcp_stats_available = false;
  uint64_t srt_received = 0, srt_lost = 0, srt_retransmitted = 0;
  uint64_t srt_base_received = 0, srt_base_lost = 0, srt_base_retransmitted = 0;
  bool srt_baseline = false;
  const uint32_t expected_run_hash = ucv_run_id_hash(opt.run_id.c_str());

  const auto t_start   = std::chrono::steady_clock::now();
  const auto t_warmup  = t_start + std::chrono::seconds(opt.warmup_s);
  const auto t_end     = t_start + std::chrono::seconds(opt.duration_s);

  std::vector<uint8_t> buf(64 * 1024);
  std::printf("[recv] listening... (Ctrl-C to stop early)\n");

  uint64_t frames_in_window = 0;
  auto     last_report = t_start;

  while (!g_quit && std::chrono::steady_clock::now() < t_end) {
    if (opt.protocol == "mjpeg") {
      Reassembler::Complete f;
      const int result = mjpeg.ReadFrame(&f, 200);
      if (result == 0) continue;
      if (result < 0) {
        std::fprintf(stderr, "warning: MJPEG stream ended\n");
        break;
      }
      if (f.run_hash != expected_run_hash) { wrong_run++; continue; }
      bytes_wire += f.wire_bytes;
      if (opt.preview_port) SendPreviewFrame(&preview, f, true);
      if (std::chrono::steady_clock::now() < t_warmup) continue;
      if (!seq.Observe(f.seq)) continue;

      if (f.tcp_stats) {
        if (!tcp_baseline) {
          tcp_base_segments = f.tcp_segments;
          tcp_base_retrans = f.tcp_retrans;
          tcp_baseline = true;
        }
        tcp_segments = f.tcp_segments - tcp_base_segments;
        tcp_retrans = f.tcp_retrans - tcp_base_retrans;
        tcp_stats_available = true;
      }
      const double transport = (static_cast<int64_t>(f.t_recv_ns) +
                                sync_before.offset_ns -
                                static_cast<int64_t>(f.t_sent_ns)) / 1e6;
      const double glass = (static_cast<int64_t>(f.t_recv_ns) +
                            sync_before.offset_ns -
                            static_cast<int64_t>(f.t_capture_ns)) / 1e6;
      transport_ms.Add(transport);
      glass_ms.Add(glass);
      encode_ms.Add(0.0);
      jitter.Update(static_cast<int64_t>(f.t_sent_ns),
                    static_cast<int64_t>(f.t_recv_ns));
      bytes_payload += f.bytes;
      frames_in_window++;
      log.WriteFrame(f.seq, f.t_capture_ns, f.t_encoded_ns, f.t_sent_ns,
                     f.t_recv_ns, f.bytes, true,
                     tcp_segments, tcp_retrans);
      const auto now = std::chrono::steady_clock::now();
      if (now - last_report >= std::chrono::seconds(5)) {
        const uint64_t tcp_attempts = tcp_segments + tcp_retrans;
        const double tcp_loss = tcp_attempts
            ? 100.0 * static_cast<double>(tcp_retrans) / tcp_attempts : 0.0;
        std::printf("[recv] %6llu MJPEG frames  transport p50=%.1f ms  jitter=%.2f ms  tcp_retrans=%.3f%%\n",
                    static_cast<unsigned long long>(frames_in_window),
                    transport_ms.Percentile(0.50), jitter.jitter_ms(),
                    tcp_loss);
        last_report = now;
      }
      continue;
    }

    int n = opt.protocol == "srt"
        ? srt.Recv(buf.data(), static_cast<int>(buf.size()))
        : video.RecvTimeout(buf.data(), buf.size(), 200);
    if (n <= 0) continue;

    const uint64_t t_recv = ucv::NowNs();
    ucv_frame_header_t h;
    Reassembler::Complete f;
    int assembled = 0;
    if (rtp_wire) {
      assembled = rtp_reasm.Push(buf.data(), static_cast<size_t>(n), t_recv,
                                 expected_run_hash, &h, &f);
      if (assembled == -2) { wrong_run++; continue; }
      if (assembled < 0) { bad_header++; continue; }
    } else {
      if (n < static_cast<int>(UCV_FRAME_HEADER_SIZE)) {
        bad_header++; continue;
      }
      std::memcpy(&h, buf.data(), sizeof(h));
      const uint16_t expected_protocol = opt.protocol == "srt"
          ? UCV_PROTO_SRT : UCV_PROTO_RAWUDP;
      if (!ucv_frame_header_valid(&h) ||
          h.protocol_id != expected_protocol) { bad_header++; continue; }
    }

    // A stale sender from a previous run would otherwise silently pollute
    // this run's statistics.
    if (h.run_id_hash != expected_run_hash) { wrong_run++; continue; }

    if (!h.fragment_count || h.fragment_index >= h.fragment_count) {
      bad_header++;
      continue;
    }
    const bool measurement_window = std::chrono::steady_clock::now() >= t_warmup;
    if (measurement_window && opt.protocol != "srt")
      packets.Observe(h.packet_seq);
    if (measurement_window && opt.protocol == "srt") {
      uint64_t received = 0, lost = 0, retransmitted = 0;
      if (srt.Stats(&received, &lost, &retransmitted)) {
        if (!srt_baseline) {
          srt_base_received = received;
          srt_base_lost = lost;
          srt_base_retransmitted = retransmitted;
          srt_baseline = true;
        }
        srt_received = received - srt_base_received;
        srt_lost = lost - srt_base_lost;
        srt_retransmitted = retransmitted - srt_base_retransmitted;
      }
    }

    bytes_wire += static_cast<uint64_t>(n);

    if (rtp_wire) {
      if (!assembled) continue;
    } else {
      const uint8_t* chunk = buf.data() + UCV_FRAME_HEADER_SIZE;
      if (!reasm.Push(h, chunk,
                      static_cast<size_t>(n) - UCV_FRAME_HEADER_SIZE,
                      t_recv, &f)) continue;
    }

    // Preview sees the initial SPS/PPS-bearing keyframe even though warmup
    // frames are intentionally excluded from all measurements below.
    if (opt.preview_port) SendPreviewFrame(&preview, f, false);

    const bool in_window = std::chrono::steady_clock::now() >= t_warmup;
    if (!in_window) continue;   // discard encoder ramp-up / handshake

    if (!seq.Observe(f.seq)) continue;  // duplicate

    // Express the PC arrival time on the phone's timebase before
    // subtracting — this is the whole reason for §4.
    const double transport = (static_cast<int64_t>(f.t_recv_ns) +
                              sync_before.offset_ns -
                              static_cast<int64_t>(f.t_sent_ns)) / 1e6;
    const double glass = (static_cast<int64_t>(f.t_recv_ns) +
                          sync_before.offset_ns -
                          static_cast<int64_t>(f.t_capture_ns)) / 1e6;
    const double enc = (static_cast<int64_t>(f.t_encoded_ns) -
                        static_cast<int64_t>(f.t_capture_ns)) / 1e6;

    transport_ms.Add(transport);
    glass_ms.Add(glass);
    encode_ms.Add(enc);
    jitter.Update(static_cast<int64_t>(f.t_sent_ns),
                  static_cast<int64_t>(f.t_recv_ns));
    bytes_payload += f.bytes;
    frames_in_window++;

    log.WriteFrame(f.seq, f.t_capture_ns, f.t_encoded_ns, f.t_sent_ns,
                   f.t_recv_ns, f.bytes, f.keyframe,
                   opt.protocol == "srt" ? srt_received : packets.received(),
                   opt.protocol == "srt" ? srt_lost : packets.missing(),
                   opt.protocol == "srt" ? srt_retransmitted : 0);

    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
      const uint64_t received_packets = opt.protocol == "srt"
          ? srt_received : packets.received();
      const uint64_t lost_packets = opt.protocol == "srt"
          ? srt_lost : packets.missing();
      const double live_loss = received_packets + lost_packets
          ? 100.0 * static_cast<double>(lost_packets) /
                (received_packets + lost_packets) : 0.0;
      std::printf("[recv] %6llu frames  transport p50=%.1f ms  jitter=%.2f ms  packet_loss=%.3f%%\n",
                  static_cast<unsigned long long>(frames_in_window),
                  transport_ms.Percentile(0.50), jitter.jitter_ms(),
                  live_loss);
      last_report = now;
    }
  }

  g_quit = true;
  if (control_thread.joinable()) control_thread.join();
  if (opt.remote_phone) {
    if (control.StopRemoteRun())
      std::printf("[phone] pipeline stopped remotely\n");
    else
      std::fprintf(stderr, "warning: phone did not ACK remote STOP\n");
  }

  // ------------------------------------------------------------------
  // 5. Clock sync AFTER the run — drift check (harness spec §4.1)
  // ------------------------------------------------------------------
  ucv::ClockSync sync_after;
  bool have_after = control.Synchronise(64, &sync_after);
  int64_t drift_ns = have_after ? (sync_after.offset_ns - sync_before.offset_ns) : 0;
  const bool clock_suspect = have_after && std::llabs(drift_ns) > 2'000'000;

  // ------------------------------------------------------------------
  // 6. Report
  // ------------------------------------------------------------------
  const double dur_s = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - t_warmup).count();

  std::printf("\n"
              "═══════════════════════════════════════════════════════════\n"
              " RESULT — %s   run_id=%s\n"
              "═══════════════════════════════════════════════════════════\n",
              opt.protocol.c_str(), opt.run_id.c_str());

  std::printf("\nTransport latency (ms)   [the ranking metric]\n"
              "  p50 %8.2f   p95 %8.2f   p99 %8.2f   max %8.2f   sd %6.2f\n",
              transport_ms.Percentile(0.50), transport_ms.Percentile(0.95),
              transport_ms.Percentile(0.99), transport_ms.Max(),
              transport_ms.StdDev());

  std::printf("\nGlass-to-glass (ms)      [product-facing, includes encode]\n"
              "  p50 %8.2f   p95 %8.2f   p99 %8.2f   max %8.2f\n",
              glass_ms.Percentile(0.50), glass_ms.Percentile(0.95),
              glass_ms.Percentile(0.99), glass_ms.Max());

  std::printf("\nCapture→encode (ms)      [phone-local, no clock sync needed]\n"
              "  p50 %8.2f   p95 %8.2f\n",
              encode_ms.Percentile(0.50), encode_ms.Percentile(0.95));

  const uint64_t reassembly_failures = reasm.failures() + rtp_reasm.failures();
  std::printf("\nReliability\n"
              "  frames received    %llu\n"
              "  sequence gaps      %llu frames in %llu events\n"
              "  reorder events     %llu\n"
              "  duplicates         %llu\n"
              "  reassembly fails   %llu\n"
              "  bad headers        %llu\n"
              "  wrong-run frames   %llu\n",
              static_cast<unsigned long long>(seq.received()),
              static_cast<unsigned long long>(seq.gap_frames()),
              static_cast<unsigned long long>(seq.gap_events()),
              static_cast<unsigned long long>(seq.reorder_events()),
              static_cast<unsigned long long>(seq.duplicates()),
              static_cast<unsigned long long>(reassembly_failures),
              static_cast<unsigned long long>(bad_header),
              static_cast<unsigned long long>(wrong_run));

  if (opt.protocol == "raw_udp" || opt.protocol == "rtp_udp" ||
      opt.protocol == "rtsp") {
    const char* packet_name = rtp_wire ? "RTP" : "UDP";
    std::printf("  %s packets recv   %llu\n"
                "  %s packets lost   %llu (%.3f %%)\n"
                "  packet reorder     %llu\n"
                "  packet duplicates  %llu\n",
                packet_name,
                static_cast<unsigned long long>(packets.received()),
                packet_name,
                static_cast<unsigned long long>(packets.missing()),
                packets.loss_pct(),
                static_cast<unsigned long long>(packets.reorder_events()),
                static_cast<unsigned long long>(packets.duplicates()));
  } else if (opt.protocol == "srt") {
    const uint64_t attempts = srt_received + srt_lost;
    const double loss = attempts
        ? 100.0 * static_cast<double>(srt_lost) / attempts : 0.0;
    std::printf("  SRT packets recv   %llu\n"
                "  SRT detected lost  %llu (%.3f %%)\n"
                "  SRT retransmitted  %llu\n"
                "  SRT profile        live/message, latency=20ms, encryption=off\n",
                static_cast<unsigned long long>(srt_received),
                static_cast<unsigned long long>(srt_lost), loss,
                static_cast<unsigned long long>(srt_retransmitted));
  } else if (opt.protocol == "mjpeg") {
    const uint64_t tcp_attempts = tcp_segments + tcp_retrans;
    const double tcp_loss = tcp_attempts
        ? 100.0 * static_cast<double>(tcp_retrans) / tcp_attempts : 0.0;
    std::printf("  TCP data segments  %llu\n"
                "  TCP retransmitted  %llu (%.3f %%)\n"
                "  TCP_INFO status    %s\n",
                static_cast<unsigned long long>(tcp_segments),
                static_cast<unsigned long long>(tcp_retrans), tcp_loss,
                tcp_stats_available ? "available" : "unavailable");
  }

  std::printf("\nJitter (RFC 3550)        [clock-offset independent]\n"
              "  %.3f ms\n", jitter.jitter_ms());

  if (dur_s > 0.0) {
    const double goodput = 8.0 * static_cast<double>(bytes_payload) / dur_s / 1e6;
    const double overhead = bytes_payload > 0
        ? 100.0 * static_cast<double>(bytes_wire - bytes_payload) /
              static_cast<double>(bytes_payload)
        : 0.0;
    std::printf("\nEfficiency\n"
                "  goodput            %.2f Mbps\n"
                "  app-layer overhead %.2f %%   (packet capture needed for true wire overhead)\n",
                goodput, overhead);
  }

  if (opt.control_load) {
    const uint64_t sent = control_sent.load(), acked = control_acked.load();
    std::printf("\nControl channel (upstream, measured UNDER video load)\n"
                "  commands sent      %llu\n"
                "  acked              %llu   (%.2f %% lost)\n",
                static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(acked),
                sent ? 100.0 * static_cast<double>(sent - acked) /
                           static_cast<double>(sent) : 0.0);
    if (!control_rtt.empty()) {
      std::printf("  RTT p50/p95/p99    %.2f / %.2f / %.2f ms   max %.2f ms\n"
                  "  [round-trip — immune to clock-sync error, trust this most]\n",
                  control_rtt.Percentile(0.50), control_rtt.Percentile(0.95),
                  control_rtt.Percentile(0.99), control_rtt.Max());
    }
    log.WriteControl(sent, acked, control_rtt.Percentile(0.50),
                     control_rtt.Percentile(0.95),
                     control_rtt.Percentile(0.99), control_rtt.Max());
  }

  std::printf("\nClock sync\n"
              "  offset before      %.3f ms\n", sync_before.offset_ns / 1e6);
  if (have_after) {
    std::printf("  offset after       %.3f ms\n"
                "  drift              %.3f ms  %s\n",
                sync_after.offset_ns / 1e6, drift_ns / 1e6,
                clock_suspect ? "*** CLOCK_SUSPECT — do not average this run away ***"
                              : "(within tolerance)");
  } else {
    std::printf("  offset after       UNAVAILABLE — run flagged CLOCK_SUSPECT\n");
  }

  std::printf("\nCaveats\n"
              "  • One-way numbers carry ±1–3 ms clock-sync error. Differences\n"
              "    below ~5 ms between protocols are NOT resolvable here.\n"
              "  • Control RTT and jitter do not depend on clock sync.\n"
              "  • True wire overhead needs a packet capture; the figure above\n"
              "    counts application bytes only and cannot see retransmits.\n");

  const bool udp_protocol = opt.protocol == "raw_udp" || rtp_wire;
  const uint64_t summary_packets_received = opt.protocol == "srt"
      ? srt_received : (udp_protocol ? packets.received() : tcp_segments);
  const uint64_t summary_packets_lost = opt.protocol == "srt"
      ? srt_lost : (udp_protocol ? packets.missing() : tcp_retrans);
  log.WriteSummary(seq.received(), seq.gap_frames(), seq.reorder_events(),
                   seq.duplicates(), reassembly_failures, bytes_payload, bytes_wire,
                   have_after ? sync_after.offset_ns : 0, drift_ns,
                   clock_suspect || !have_after,
                   summary_packets_received, summary_packets_lost,
                   packets.reorder_events(), packets.duplicates(),
                   opt.protocol == "srt" ? srt_retransmitted : 0);
  log.Close();

  std::printf("\nlog written: %s/receiver-%s.ndjson\n",
              opt.out_dir.c_str(), opt.run_id.c_str());

  ucv::NetShutdown();
  return 0;
}
