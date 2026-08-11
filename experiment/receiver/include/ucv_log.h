// NDJSON run logger.
//
// Records RAW timestamps only — never derived latencies. A correction to a
// metric formula can then be re-applied to every historical run without
// re-measuring, which matters because re-measuring means re-booking the rig,
// the camera, and the network conditions.

#ifndef UCV_LOG_H
#define UCV_LOG_H

#include <cstdint>
#include <cstdio>
#include <string>

namespace ucv {

class NdjsonWriter {
 public:
  explicit NdjsonWriter(const std::string& path);
  ~NdjsonWriter();
  NdjsonWriter(const NdjsonWriter&)            = delete;
  NdjsonWriter& operator=(const NdjsonWriter&) = delete;

  bool ok() const { return f_ != nullptr; }

  void WriteMeta(const std::string& run_id, const std::string& protocol,
                 const std::string& mode, const std::string& condition,
                 int64_t clock_offset_ns, int64_t best_rtt_ns);

  void WriteFrame(uint32_t seq, uint64_t cap_ns, uint64_t enc_ns,
                  uint64_t snd_ns, uint64_t rcv_ns, uint32_t bytes,
                  bool keyframe, uint64_t packets_received = 0,
                  uint64_t packets_lost = 0);

  void WriteControl(uint64_t sent, uint64_t acked, double p50_ms,
                    double p95_ms, double p99_ms, double max_ms);

  void WriteSummary(uint64_t frames_received, uint64_t gap_frames,
                    uint64_t reorder_events, uint64_t duplicates,
                    uint64_t reassembly_failures, uint64_t bytes_payload,
                    uint64_t bytes_wire, int64_t offset_after_ns,
                    int64_t drift_ns, bool clock_suspect,
                    uint64_t packets_received = 0,
                    uint64_t packets_lost = 0,
                    uint64_t packet_reorder_events = 0,
                    uint64_t packet_duplicates = 0);

  void Close();

 private:
  std::FILE* f_ = nullptr;
  unsigned frames_since_flush_ = 0;
};

}  // namespace ucv

#endif  // UCV_LOG_H
