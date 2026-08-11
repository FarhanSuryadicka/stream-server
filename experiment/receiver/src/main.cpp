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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
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
      "  --protocol <name>     raw_udp | mjpeg  (default: raw_udp)\n"
      "  --video-port <n>      Video port      (default: protocol default)\n"
      "  --control-port <n>    Control port    (default: %d)\n"
      "  --duration <s>        Run length      (default: 120)\n"
      "  --warmup <s>          Discarded head  (default: 10)\n"
      "  --run-id <id>         Run identifier  (default: auto)\n"
      "  --out <dir>           Log directory   (default: .)\n"
      "  --mode <WxH@fps>      Capture mode metadata (default: unknown)\n"
      "  --condition <name>    Network condition label (default: unspecified)\n"
      "  --preview-port <n>    Mirror H.264 frames to localhost dashboard\n"
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

void SendPreviewFrame(ucv::UdpSender* sender, const Reassembler::Complete& frame) {
  if (!sender || frame.payload.empty()) return;
  const size_t count = (frame.payload.size() + kPreviewChunk - 1) / kPreviewChunk;
  if (count == 0 || count > 65535) return;
  std::vector<uint8_t> packet(sizeof(PreviewHeader) + kPreviewChunk);
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = i * kPreviewChunk;
    const size_t bytes = std::min(kPreviewChunk, frame.payload.size() - offset);
    PreviewHeader h{kPreviewMagic, frame.seq,
                    static_cast<uint32_t>(frame.payload.size()),
                    static_cast<uint16_t>(i), static_cast<uint16_t>(count)};
    std::memcpy(packet.data(), &h, sizeof(h));
    std::memcpy(packet.data() + sizeof(h), frame.payload.data() + offset, bytes);
    if (!sender->Send(packet.data(), sizeof(h) + bytes)) break;
  }
}

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
  ucv::NdjsonWriter log(opt.out_dir + "/receiver-" + opt.run_id + ".ndjson");
  if (!log.ok()) {
    std::fprintf(stderr, "fatal: cannot write log in '%s'\n", opt.out_dir.c_str());
    return 1;
  }
  log.WriteMeta(opt.run_id, opt.protocol, opt.mode, opt.condition,
                sync_before.offset_ns,
                sync_before.best_rtt_ns);

  ucv::UdpSocket video;
  ucv::UdpSender preview;
  if (opt.protocol == "raw_udp") {
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
    if (!control.SetRunHash(opt.run_id) ||
        !control.StartRemoteRun(mode_w, mode_h, mode_fps,
                                UCV_PROTO_RAWUDP, opt.video_port)) {
      std::fprintf(stderr,
                   "fatal: phone rejected remote START for %s; refresh camera "
                   "modes and check the phone log\n",
                   opt.mode.c_str());
      return 2;
    }
    std::printf("[phone] pipeline started remotely: %s -> this PC:%d\n",
                opt.mode.c_str(), opt.video_port);
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
  ucv::JitterEstimator jitter;
  ucv::Distribution    transport_ms, glass_ms, encode_ms;
  Reassembler          reasm;

  uint64_t bytes_payload = 0, bytes_wire = 0;
  uint64_t bad_header = 0, wrong_run = 0;
  const uint32_t expected_run_hash = ucv_run_id_hash(opt.run_id.c_str());

  const auto t_start   = std::chrono::steady_clock::now();
  const auto t_warmup  = t_start + std::chrono::seconds(opt.warmup_s);
  const auto t_end     = t_start + std::chrono::seconds(opt.duration_s);

  std::vector<uint8_t> buf(64 * 1024);
  std::printf("[recv] listening... (Ctrl-C to stop early)\n");

  uint64_t frames_in_window = 0;
  auto     last_report = t_start;

  while (!g_quit && std::chrono::steady_clock::now() < t_end) {
    int n = video.RecvTimeout(buf.data(), buf.size(), 200);
    if (n <= 0) continue;

    const uint64_t t_recv = ucv::NowNs();
    if (n < static_cast<int>(UCV_FRAME_HEADER_SIZE)) { bad_header++; continue; }

    ucv_frame_header_t h;
    std::memcpy(&h, buf.data(), sizeof(h));
    if (!ucv_frame_header_valid(&h)) { bad_header++; continue; }

    // A stale sender from a previous run would otherwise silently pollute
    // this run's statistics.
    if (h.run_id_hash != expected_run_hash) { wrong_run++; continue; }

    bytes_wire += static_cast<uint64_t>(n);

    Reassembler::Complete f;
    const uint8_t* chunk = buf.data() + UCV_FRAME_HEADER_SIZE;
    if (!reasm.Push(h, chunk,
                    static_cast<size_t>(n) - UCV_FRAME_HEADER_SIZE, t_recv, &f))
      continue;

    // Preview sees the initial SPS/PPS-bearing keyframe even though warmup
    // frames are intentionally excluded from all measurements below.
    if (opt.preview_port) SendPreviewFrame(&preview, f);

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
                   f.t_recv_ns, f.bytes, f.keyframe);

    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
      std::printf("[recv] %6llu frames  transport p50=%.1f ms  jitter=%.2f ms  gaps=%llu\n",
                  static_cast<unsigned long long>(frames_in_window),
                  transport_ms.Percentile(0.50), jitter.jitter_ms(),
                  static_cast<unsigned long long>(seq.gap_frames()));
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
              static_cast<unsigned long long>(reasm.failures()),
              static_cast<unsigned long long>(bad_header),
              static_cast<unsigned long long>(wrong_run));

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

  log.WriteSummary(seq.received(), seq.gap_frames(), seq.reorder_events(),
                   seq.duplicates(), reasm.failures(), bytes_payload, bytes_wire,
                   have_after ? sync_after.offset_ns : 0, drift_ns,
                   clock_suspect || !have_after);
  log.Close();

  std::printf("\nlog written: %s/receiver-%s.ndjson\n",
              opt.out_dir.c_str(), opt.run_id.c_str());

  ucv::NetShutdown();
  return 0;
}
