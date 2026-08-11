#include "ucv_log.h"

#ifdef _WIN32
#include <share.h>  // _SH_DENYWR for the shared-read open below
#endif

namespace ucv {

namespace {

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c >= 0x20) out += static_cast<char>(c);
        break;
    }
  }
  return out;
}

}  // namespace

NdjsonWriter::NdjsonWriter(const std::string& path) {
#ifdef _WIN32
  // Share the log for reading while the run is in progress.
  //
  // fopen/fopen_s open with exclusive access on Windows, which locks the whole
  // file for the lifetime of the run. The dashboard polls this file to draw the
  // live charts, and against an exclusive writer every read failed with EACCES
  // until the receiver exited — so the graphs stayed empty until the operator
  // pressed Stop. Sharing is the writer's decision: a reader cannot opt in on
  // its own, so it has to be granted here.
  //
  // _SH_DENYWR still denies other WRITERS, so a second receiver cannot
  // interleave lines into the same log and corrupt a measurement.
  f_ = _fsopen(path.c_str(), "wb", _SH_DENYWR);
#else
  f_ = std::fopen(path.c_str(), "wb");
#endif
}

NdjsonWriter::~NdjsonWriter() { Close(); }

void NdjsonWriter::WriteMeta(const std::string& run_id,
                             const std::string& protocol,
                             const std::string& mode,
                             const std::string& condition,
                             int64_t clock_offset_ns, int64_t best_rtt_ns) {
  if (!f_) return;
  const std::string safe_run = JsonEscape(run_id);
  const std::string safe_protocol = JsonEscape(protocol);
  const std::string safe_mode = JsonEscape(mode);
  const std::string safe_condition = JsonEscape(condition);
  std::fprintf(f_,
               "{\"type\":\"meta\",\"run_id\":\"%s\",\"protocol\":\"%s\"," 
               "\"mode\":\"%s\",\"condition\":\"%s\"," 
               "\"clock_offset_ns\":%lld,\"clock_best_rtt_ns\":%lld}\n",
               safe_run.c_str(), safe_protocol.c_str(), safe_mode.c_str(),
               safe_condition.c_str(),
               static_cast<long long>(clock_offset_ns),
               static_cast<long long>(best_rtt_ns));
}

void NdjsonWriter::WriteFrame(uint32_t seq, uint64_t cap_ns, uint64_t enc_ns,
                              uint64_t snd_ns, uint64_t rcv_ns, uint32_t bytes,
                              bool keyframe, uint64_t packets_received,
                              uint64_t packets_lost) {
  if (!f_) return;
  std::fprintf(f_,
               "{\"type\":\"frame\",\"seq\":%u,\"cap_ns\":%llu,\"enc_ns\":%llu,"
               "\"snd_ns\":%llu,\"rcv_ns\":%llu,\"bytes\":%u,\"key\":%s,"
               "\"packets_received\":%llu,\"packets_lost\":%llu}\n",
               seq, static_cast<unsigned long long>(cap_ns),
               static_cast<unsigned long long>(enc_ns),
               static_cast<unsigned long long>(snd_ns),
               static_cast<unsigned long long>(rcv_ns), bytes,
               keyframe ? "true" : "false",
               static_cast<unsigned long long>(packets_received),
               static_cast<unsigned long long>(packets_lost));
  // Keep the active-run dashboard within roughly half a second at 20 fps,
  // while avoiding an fflush syscall for every received packet/frame.
  if (++frames_since_flush_ >= 10) {
    std::fflush(f_);
    frames_since_flush_ = 0;
  }
}

void NdjsonWriter::WriteControl(uint64_t sent, uint64_t acked, double p50_ms,
                                double p95_ms, double p99_ms, double max_ms) {
  if (!f_) return;
  std::fprintf(f_,
               "{\"type\":\"control\",\"sent\":%llu,\"acked\":%llu,"
               "\"p50_ms\":%.6f,\"p95_ms\":%.6f,\"p99_ms\":%.6f,"
               "\"max_ms\":%.6f}\n",
               static_cast<unsigned long long>(sent),
               static_cast<unsigned long long>(acked), p50_ms, p95_ms,
               p99_ms, max_ms);
}

void NdjsonWriter::WriteSummary(uint64_t frames_received, uint64_t gap_frames,
                                uint64_t reorder_events, uint64_t duplicates,
                                uint64_t reassembly_failures,
                                uint64_t bytes_payload, uint64_t bytes_wire,
                                int64_t offset_after_ns, int64_t drift_ns,
                                bool clock_suspect, uint64_t packets_received,
                                uint64_t packets_lost,
                                uint64_t packet_reorder_events,
                                uint64_t packet_duplicates) {
  if (!f_) return;
  std::fprintf(f_,
               "{\"type\":\"summary\",\"frames_received\":%llu,"
               "\"gap_frames\":%llu,\"reorder_events\":%llu,"
               "\"duplicates\":%llu,\"reassembly_failures\":%llu,"
               "\"bytes_payload\":%llu,\"bytes_wire\":%llu,"
               "\"packets_received\":%llu,\"packets_lost\":%llu,"
               "\"packet_reorder_events\":%llu,\"packet_duplicates\":%llu,"
               "\"clock_offset_after_ns\":%lld,\"clock_drift_ns\":%lld,"
               "\"clock_status\":\"%s\"}\n",
               static_cast<unsigned long long>(frames_received),
               static_cast<unsigned long long>(gap_frames),
               static_cast<unsigned long long>(reorder_events),
               static_cast<unsigned long long>(duplicates),
               static_cast<unsigned long long>(reassembly_failures),
               static_cast<unsigned long long>(bytes_payload),
               static_cast<unsigned long long>(bytes_wire),
               static_cast<unsigned long long>(packets_received),
               static_cast<unsigned long long>(packets_lost),
               static_cast<unsigned long long>(packet_reorder_events),
               static_cast<unsigned long long>(packet_duplicates),
               static_cast<long long>(offset_after_ns),
               static_cast<long long>(drift_ns),
               clock_suspect ? "CLOCK_SUSPECT" : "OK");
}

void NdjsonWriter::Close() {
  if (f_) {
    std::fclose(f_);
    f_ = nullptr;
  }
}

}  // namespace ucv
