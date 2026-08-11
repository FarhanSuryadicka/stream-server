// Parsed representation of one measurement run.
//
// The NDJSON on disk is the single source of truth, shared with the Python
// dashboard and written by ucv-receiver. This header describes what a reader
// may rely on, and nothing more: fields the receiver does not write are
// optional, so a log from an older build still parses instead of being
// rejected wholesale.

#ifndef UCV_RUN_DATA_HPP
#define UCV_RUN_DATA_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucv {

// One frame as recorded. RAW timestamps only — never derived latencies, so a
// corrected formula can be re-applied to historical runs without re-measuring
// (harness spec §8). Everything in Metrics is computed from these.
struct FrameRecord {
  std::uint32_t seq = 0;
  std::uint64_t cap_ns = 0;
  std::uint64_t enc_ns = 0;
  std::uint64_t snd_ns = 0;
  std::uint64_t rcv_ns = 0;
  std::uint32_t bytes = 0;
  bool          keyframe = false;
  // Present only for protocols that expose packet counters (raw UDP, RTP, SRT).
  std::uint64_t packets_received = 0;
  std::uint64_t packets_lost = 0;
  std::uint64_t packets_recovered = 0;
};

struct ControlRecord {
  std::uint64_t sent = 0;
  std::uint64_t acked = 0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
};

struct SummaryRecord {
  std::uint64_t frames_received = 0;
  std::uint64_t gap_frames = 0;
  std::uint64_t reorder_events = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t reassembly_failures = 0;
  std::uint64_t bytes_payload = 0;
  std::uint64_t bytes_wire = 0;
  std::uint64_t measurement_duration_ns = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t packets_lost = 0;
  std::uint64_t packets_recovered = 0;
  std::uint64_t packet_reorder_events = 0;
  std::uint64_t packet_duplicates = 0;
  std::int64_t  clock_offset_after_ns = 0;
  std::int64_t  clock_drift_ns = 0;
  std::string   clock_status;   // "OK" | "CLOCK_SUSPECT"
};

struct MetaRecord {
  std::string run_id;
  std::string protocol;
  std::string mode;         // "1280x720@30"
  std::string condition;    // "clean"
  std::int64_t clock_offset_ns = 0;
  std::int64_t clock_best_rtt_ns = 0;
};

// A run as read from disk. A run still being written has no summary — that is
// the normal in-progress state, not an error, and the UI must show it live.
struct RunLog {
  std::string   file;          // path relative to the results directory
  MetaRecord    meta;
  std::vector<FrameRecord> frames;
  std::optional<ControlRecord> control;
  std::optional<SummaryRecord> summary;
  std::size_t   parse_errors = 0;   // truncated tail while streaming: expected
  std::optional<std::string> read_error;  // could not open at all
};

}  // namespace ucv

#endif  // UCV_RUN_DATA_HPP
