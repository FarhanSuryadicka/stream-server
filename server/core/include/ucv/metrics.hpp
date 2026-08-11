// Derived metrics for one run.
//
// These formulas are duplicated in experiment/dashboard/server.py. That
// duplication is unavoidable — two dashboards in two languages — so it is made
// safe instead: every definition is stated once here, mirrored there, and
// pinned by core/tests/parity_test.cpp. If the two ever disagree about the same
// NDJSON file, a test fails rather than an operator comparing two numbers and
// having to guess which is right.

#ifndef UCV_METRICS_HPP
#define UCV_METRICS_HPP

#include "ucv/run_data.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucv {

// What "loss" means for a protocol. Reporting one definition for all of them
// would be wrong: a reliable transport can show packet loss while frame gaps
// stay zero, because retransmission worked (handoff §10).
enum class LossBasis {
  UdpPackets,        // raw_udp: missing application UDP packet_seq
  RtpPackets,        // rtp_udp / rtsp: missing RTP sequence
  SrtDetected,       // srt: libsrt detected-missing; recovered counted apart
  TcpRetransmits,    // mjpeg: retransmission rate from TCP_INFO
  Frames,            // fallback: gaps in frame_seq seen by the receiver
};

const char* to_string(LossBasis basis);

// Human-readable label for the loss figure, so a chart axis cannot imply the
// wrong thing.
const char* loss_label(LossBasis basis);

struct Percentiles {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double max = 0.0;
};

// One point on the live charts. `t_seconds` is elapsed time since the run's
// first received frame, derived from rcv_ns (PC clock) so it needs no
// clock-offset correction.
struct SeriesPoint {
  std::uint32_t index = 0;
  std::uint32_t seq = 0;
  double t_seconds = 0.0;
  double latency_ms = 0.0;
  double jitter_ms = 0.0;
  double loss_percent = 0.0;
  std::uint32_t bytes = 0;
};

struct RunMetrics {
  std::string file;
  std::string run_id;
  std::string protocol;
  std::string mode;
  std::string condition;
  std::string group;          // operator-assigned comparison bucket, may be empty
  std::string clock_status;   // "OK" | "CLOCK_SUSPECT" | "RUNNING" | "INCOMPLETE"

  bool valid = false;         // clock OK and at least one frame
  std::size_t frames = 0;
  double fps = 0.0;

  Percentiles transport_ms;
  Percentiles glass_ms;
  Percentiles encode_ms;

  double jitter_ms = 0.0;
  std::uint64_t gaps = 0;
  double loss_percent = 0.0;
  LossBasis loss_basis = LossBasis::Frames;

  double goodput_mbps = 0.0;
  // Absent when the protocol's framing is below the receive API and no wire
  // total is observable (WebRTC). Printing a computed number from a zero would
  // claim -100 % overhead — a measurement that was never taken.
  std::optional<double> overhead_percent;

  std::optional<ControlRecord> control;
  std::vector<SeriesPoint> series;   // empty unless requested
};

// Percentile with linear interpolation, matching the Python implementation.
double percentile(std::vector<double> values, double p);

// Computes everything above. `include_series` is opt-in because the series is
// the expensive part and the run list does not need it.
RunMetrics compute_metrics(const RunLog& log, bool include_series);

// Reduces a series to at most `limit` points for display. Keeps whole points
// (not averages) so a spike stays visible at its real time position.
std::vector<SeriesPoint> downsample(const std::vector<SeriesPoint>& points,
                                    std::size_t limit = 320);

}  // namespace ucv

#endif  // UCV_METRICS_HPP
