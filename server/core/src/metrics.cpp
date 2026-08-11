#include "ucv/metrics.hpp"

#include <algorithm>
#include <cmath>

namespace ucv {

const char* to_string(LossBasis basis) {
  switch (basis) {
    case LossBasis::UdpPackets:     return "udp_packets";
    case LossBasis::RtpPackets:     return "rtp_packets";
    case LossBasis::SrtDetected:    return "srt_detected_packets";
    case LossBasis::TcpRetransmits: return "tcp_retransmissions";
    case LossBasis::Frames:         return "frames";
  }
  return "frames";
}

const char* loss_label(LossBasis basis) {
  switch (basis) {
    case LossBasis::UdpPackets:     return "Observed UDP packet loss";
    case LossBasis::RtpPackets:     return "Observed RTP packet loss";
    case LossBasis::SrtDetected:    return "SRT detected packet loss";
    case LossBasis::TcpRetransmits: return "TCP retransmission rate";
    case LossBasis::Frames:         return "Observed frame loss";
  }
  return "Observed frame loss";
}

// Linear interpolation between neighbours, identical to the Python
// implementation. Kept byte-for-byte equivalent rather than "close enough":
// the two dashboards must not report different p99 for the same log.
double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double idx = p * static_cast<double>(values.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi) return values[lo];
  const double frac = idx - static_cast<double>(lo);
  return values[lo] * (1.0 - frac) + values[hi] * frac;
}

namespace {

Percentiles summarise(const std::vector<double>& v) {
  Percentiles p;
  if (v.empty()) return p;
  p.p50 = percentile(v, 0.50);
  p.p95 = percentile(v, 0.95);
  p.p99 = percentile(v, 0.99);
  p.max = *std::max_element(v.begin(), v.end());
  return p;
}

}  // namespace

RunMetrics compute_metrics(const RunLog& log, bool include_series) {
  RunMetrics m;
  m.file = log.file;
  m.run_id = log.meta.run_id;
  m.protocol = log.meta.protocol.empty() ? "unknown" : log.meta.protocol;
  m.mode = log.meta.mode.empty() ? "unknown" : log.meta.mode;
  m.condition = log.meta.condition.empty() ? "unspecified" : log.meta.condition;
  m.control = log.control;
  m.frames = log.frames.size();

  const double offset = static_cast<double>(log.meta.clock_offset_ns);

  std::vector<double> transport, glass, encode;
  transport.reserve(log.frames.size());
  glass.reserve(log.frames.size());
  encode.reserve(log.frames.size());

  double jitter_ns = 0.0;
  bool have_prev = false;
  std::uint64_t prev_snd = 0, prev_rcv = 0;
  std::uint64_t running_gaps = 0, running_received = 0;
  bool have_prev_seq = false;
  std::uint32_t prev_seq = 0;

  const std::uint64_t first_rcv =
      log.frames.empty() ? 0 : log.frames.front().rcv_ns;

  for (std::size_t i = 0; i < log.frames.size(); ++i) {
    const FrameRecord& f = log.frames[i];
    const double trans_ms =
        (static_cast<double>(f.rcv_ns) + offset - static_cast<double>(f.snd_ns)) / 1e6;
    const double glass_ms =
        (static_cast<double>(f.rcv_ns) + offset - static_cast<double>(f.cap_ns)) / 1e6;
    const double enc_ms =
        (static_cast<double>(f.enc_ns) - static_cast<double>(f.cap_ns)) / 1e6;
    transport.push_back(trans_ms);
    glass.push_back(glass_ms);
    encode.push_back(enc_ms);

    // RFC 3550 estimator: independent of the absolute clock offset, which is
    // why it is trusted more than the one-way figures.
    if (have_prev) {
      const double d = std::fabs(
          (static_cast<double>(f.rcv_ns) - static_cast<double>(prev_rcv)) -
          (static_cast<double>(f.snd_ns) - static_cast<double>(prev_snd)));
      jitter_ns += (d - jitter_ns) / 16.0;
    }
    prev_snd = f.snd_ns;
    prev_rcv = f.rcv_ns;
    have_prev = true;

    // Sequence gaps, guarding the 32-bit wrap the same way the receiver does.
    if (have_prev_seq) {
      const std::uint32_t delta = f.seq - prev_seq;
      if (delta > 1 && delta < 0x80000000u) running_gaps += delta - 1;
    }
    prev_seq = f.seq;
    have_prev_seq = true;
    running_received++;

    if (include_series) {
      SeriesPoint pt;
      pt.index = static_cast<std::uint32_t>(i + 1);
      pt.seq = f.seq;
      pt.t_seconds =
          static_cast<double>(f.rcv_ns - first_rcv) / 1e9;
      pt.latency_ms = trans_ms;
      pt.jitter_ms = jitter_ns / 1e6;
      // Per-frame loss uses whichever counter the protocol actually provides,
      // matching the run-level basis chosen below.
      if (f.packets_received + f.packets_lost > 0) {
        pt.loss_percent = 100.0 * static_cast<double>(f.packets_lost) /
                          static_cast<double>(f.packets_received + f.packets_lost);
      } else {
        const std::uint64_t denom = running_received + running_gaps;
        pt.loss_percent = denom ? 100.0 * static_cast<double>(running_gaps) /
                                      static_cast<double>(denom)
                                : 0.0;
      }
      pt.bytes = f.bytes;
      m.series.push_back(pt);
    }
  }

  m.transport_ms = summarise(transport);
  m.glass_ms = summarise(glass);
  m.encode_ms = summarise(encode);
  m.jitter_ms = jitter_ns / 1e6;

  double duration_s = 0.0;
  if (log.frames.size() > 1) {
    duration_s = static_cast<double>(log.frames.back().rcv_ns -
                                     log.frames.front().rcv_ns) / 1e9;
  }
  m.fps = duration_s > 0.0
              ? static_cast<double>(log.frames.size()) / duration_s
              : 0.0;

  std::uint64_t payload = 0, wire = 0, received = log.frames.size();
  std::uint64_t gaps = running_gaps;
  std::uint64_t packets_received = 0, packets_lost = 0;

  if (log.summary) {
    const SummaryRecord& s = *log.summary;
    payload = s.bytes_payload;
    wire = s.bytes_wire;
    received = s.frames_received;
    gaps = s.gap_frames;
    packets_received = s.packets_received;
    packets_lost = s.packets_lost;
    m.clock_status = s.clock_status.empty() ? "OK" : s.clock_status;
  } else {
    for (const FrameRecord& f : log.frames) payload += f.bytes;
    if (!log.frames.empty()) {
      packets_received = log.frames.back().packets_received;
      packets_lost = log.frames.back().packets_lost;
    }
    // No summary line yet: either still streaming, or the receiver was killed
    // before it could write one. Both are "not a valid run", but the UI must
    // distinguish "in progress" from "abandoned".
    m.clock_status = log.frames.empty() ? "INCOMPLETE" : "RUNNING";
  }

  m.gaps = gaps;
  m.valid = (m.clock_status == "OK") && !log.frames.empty();

  const std::uint64_t packet_total = packets_received + packets_lost;
  const std::string& proto = m.protocol;
  if (proto == "raw_udp" && packet_total) {
    m.loss_percent = 100.0 * static_cast<double>(packets_lost) /
                     static_cast<double>(packet_total);
    m.loss_basis = LossBasis::UdpPackets;
  } else if ((proto == "rtp_udp" || proto == "rtsp") && packet_total) {
    m.loss_percent = 100.0 * static_cast<double>(packets_lost) /
                     static_cast<double>(packet_total);
    m.loss_basis = LossBasis::RtpPackets;
  } else if (proto == "srt" && packet_total) {
    m.loss_percent = 100.0 * static_cast<double>(packets_lost) /
                     static_cast<double>(packet_total);
    m.loss_basis = LossBasis::SrtDetected;
  } else if (proto == "mjpeg" && packet_total) {
    m.loss_percent = 100.0 * static_cast<double>(packets_lost) /
                     static_cast<double>(packet_total);
    m.loss_basis = LossBasis::TcpRetransmits;
  } else {
    const std::uint64_t expected = received + gaps;
    m.loss_percent = expected ? 100.0 * static_cast<double>(gaps) /
                                    static_cast<double>(expected)
                              : 0.0;
    m.loss_basis = LossBasis::Frames;
  }

  m.goodput_mbps = duration_s > 0.0
                       ? 8.0 * static_cast<double>(payload) / duration_s / 1e6
                       : 0.0;
  // Absent, not zero, when the protocol reports no wire total. See the note on
  // RunMetrics::overhead_percent.
  if (payload > 0 && wire > 0) {
    m.overhead_percent = 100.0 *
                         (static_cast<double>(wire) - static_cast<double>(payload)) /
                         static_cast<double>(payload);
  }

  if (include_series) m.series = downsample(m.series);
  return m;
}

std::vector<SeriesPoint> downsample(const std::vector<SeriesPoint>& points,
                                    std::size_t limit) {
  if (points.size() <= limit) return points;
  std::vector<SeriesPoint> out;
  out.reserve(limit);
  const double step = static_cast<double>(points.size() - 1) /
                      static_cast<double>(limit - 1);
  for (std::size_t i = 0; i < limit; ++i) {
    out.push_back(points[static_cast<std::size_t>(std::lround(
        static_cast<double>(i) * step))]);
  }
  return out;
}

}  // namespace ucv
