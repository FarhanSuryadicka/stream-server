// Host test for monitor-core. No Qt, no display, no camera, no network.
//
// Its job is to stop the two dashboards drifting apart. Both compute metrics
// from the same NDJSON, in different languages; if one changes a formula the
// operator would see two different p99 for one run and have no way to tell
// which is right. The expectations below are computed by hand from fixed
// inputs, so they pin the definition rather than whatever the code currently
// happens to produce.

#include "ucv/metrics.hpp"
#include "ucv/ndjson_reader.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
  std::printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
  if (!ok && !detail.empty()) std::printf("  <- %s", detail.c_str());
  std::printf("\n");
  if (!ok) ++g_failures;
}

void check_near(const char* what, double got, double want, double tol) {
  const bool ok = std::fabs(got - want) <= tol;
  char detail[128];
  std::snprintf(detail, sizeof(detail), "got %.6f want %.6f", got, want);
  check(what, ok, ok ? "" : detail);
}

std::filesystem::path temp_dir() {
  auto dir = std::filesystem::temp_directory_path() / "ucv-monitor-test";
  std::filesystem::create_directories(dir);
  return dir;
}

// Writes a run whose numbers are chosen so every expectation can be derived by
// hand rather than copied from a previous run of this test.
std::filesystem::path write_fixture(const std::filesystem::path& dir) {
  const auto path = dir / "receiver-PARITY.ndjson";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << R"({"type":"meta","run_id":"PARITY","protocol":"raw_udp",)"
      << R"("mode":"640x480@30","condition":"clean",)"
      << R"("clock_offset_ns":1000,"clock_best_rtt_ns":2000})" << "\n";
  // Five frames, transport latency 1..5 ms exactly (rcv + offset - snd).
  for (int i = 0; i < 5; ++i) {
    const long long cap = 1000000LL * (i + 1);
    const long long enc = cap + 2000000LL;              // encode 2 ms
    const long long snd = enc + 1000000LL;              // +1 ms
    const long long rcv = snd - 1000 + 1000000LL * (i + 1);  // 1..5 ms, offset 1000
    out << R"({"type":"frame","seq":)" << (i + 1)
        << R"(,"cap_ns":)" << cap
        << R"(,"enc_ns":)" << enc
        << R"(,"snd_ns":)" << snd
        << R"(,"rcv_ns":)" << rcv
        << R"(,"bytes":1000,"key":)" << (i == 0 ? "true" : "false")
        << R"(,"packets_received":0,"packets_lost":0,"packets_recovered":0})" << "\n";
  }
  out << R"({"type":"control","sent":10,"acked":10,"p50_ms":1.5,)"
      << R"("p95_ms":2.5,"p99_ms":3.5,"max_ms":4.0})" << "\n";
  out << R"({"type":"summary","frames_received":5,"gap_frames":0,)"
      << R"("reorder_events":0,"duplicates":0,"reassembly_failures":0,)"
      << R"("bytes_payload":5000,"bytes_wire":5250,)"
      << R"("measurement_duration_ns":5000000,)"
      << R"("packets_received":0,"packets_lost":0,"packets_recovered":0,)"
      << R"("packet_reorder_events":0,"packet_duplicates":0,)"
      << R"("clock_offset_after_ns":1000,"clock_drift_ns":0,)"
      << R"("clock_status":"OK"})" << "\n";
  return path;
}

}  // namespace

int main() {
  std::printf("monitor-core — metric parity with the Python dashboard\n");
  std::printf("=====================================================\n");

  const auto dir = temp_dir();
  const auto file = write_fixture(dir);

  std::printf("\n[percentile] linear interpolation must match Python's\n");
  // Python: idx = p*(n-1); interpolate between floor and ceil.
  check_near("p50 of 1..5 is 3", ucv::percentile({1, 2, 3, 4, 5}, 0.50), 3.0, 1e-9);
  check_near("p95 of 1..5 is 4.8", ucv::percentile({1, 2, 3, 4, 5}, 0.95), 4.8, 1e-9);
  check_near("p99 of 1..5 is 4.96", ucv::percentile({1, 2, 3, 4, 5}, 0.99), 4.96, 1e-9);
  check_near("empty input yields 0", ucv::percentile({}, 0.5), 0.0, 1e-9);
  check_near("single value is itself", ucv::percentile({7.5}, 0.99), 7.5, 1e-9);

  std::printf("\n[reader] a complete run parses fully\n");
  const ucv::RunLog log = ucv::read_run_log(file, dir);
  check("no read error", !log.read_error.has_value());
  check("meta parsed", log.meta.run_id == "PARITY" && log.meta.protocol == "raw_udp",
        log.meta.run_id);
  check("mode and condition parsed",
        log.meta.mode == "640x480@30" && log.meta.condition == "clean");
  check("clock offset parsed", log.meta.clock_offset_ns == 1000);
  check("all five frames parsed", log.frames.size() == 5,
        std::to_string(log.frames.size()));
  check("control record parsed", log.control.has_value());
  check("summary record parsed", log.summary.has_value());
  check("summary clock status is OK",
        log.summary && log.summary->clock_status == "OK");
  check("keyframe flag parsed", !log.frames.empty() && log.frames[0].keyframe);
  check("no spurious parse errors", log.parse_errors == 0,
        std::to_string(log.parse_errors));

  std::printf("\n[metrics] derived values match the documented formulas\n");
  const ucv::RunMetrics m = ucv::compute_metrics(log, true);
  check("run is valid (clock OK and frames present)", m.valid);
  check("frame count", m.frames == 5);
  check_near("transport p50 is 3 ms", m.transport_ms.p50, 3.0, 1e-6);
  check_near("transport max is 5 ms", m.transport_ms.max, 5.0, 1e-6);
  check_near("encode p50 is 2 ms", m.encode_ms.p50, 2.0, 1e-6);
  // glass = rcv + offset - cap = transport + (snd - cap) = transport + 3 ms
  check_near("glass p50 is transport + encode + 1 ms", m.glass_ms.p50, 6.0, 1e-6);
  check("loss basis falls back to frames when no packet counters",
        m.loss_basis == ucv::LossBasis::Frames, ucv::to_string(m.loss_basis));
  check_near("loss is zero with no gaps", m.loss_percent, 0.0, 1e-9);
  check("overhead is present when a wire total exists",
        m.overhead_percent.has_value());
  // (5250 - 5000) / 5000 = 5 %
  if (m.overhead_percent)
    check_near("overhead is 5 %", *m.overhead_percent, 5.0, 1e-6);

  std::printf("\n[metrics] a protocol with no wire total reports no overhead\n");
  {
    ucv::RunLog l = log;
    l.summary->bytes_wire = 0;          // WebRTC: framing below the API
    const ucv::RunMetrics wm = ucv::compute_metrics(l, false);
    check("overhead absent rather than -100 %", !wm.overhead_percent.has_value());
  }

  std::printf("\n[metrics] packet counters override the frame fallback\n");
  {
    ucv::RunLog l = log;
    l.summary->packets_received = 900;
    l.summary->packets_lost = 100;
    const ucv::RunMetrics pm = ucv::compute_metrics(l, false);
    check("basis becomes udp_packets for raw_udp",
          pm.loss_basis == ucv::LossBasis::UdpPackets);
    check_near("loss is 10 %", pm.loss_percent, 10.0, 1e-9);
  }

  std::printf("\n[metrics] a run still streaming is RUNNING, not valid\n");
  {
    ucv::RunLog l = log;
    l.summary.reset();
    const ucv::RunMetrics rm = ucv::compute_metrics(l, false);
    check("clock status is RUNNING", rm.clock_status == "RUNNING", rm.clock_status);
    check("not counted as valid", !rm.valid);
    check("frames still reported", rm.frames == 5);
  }

  std::printf("\n[series] time axis starts at zero and is monotonic\n");
  check("series produced", !m.series.empty());
  if (!m.series.empty()) {
    check_near("first point at t=0", m.series.front().t_seconds, 0.0, 1e-9);
    bool monotonic = true;
    for (std::size_t i = 1; i < m.series.size(); ++i)
      if (m.series[i].t_seconds < m.series[i - 1].t_seconds) monotonic = false;
    check("time is non-decreasing", monotonic);
  }

  std::printf("\n[series] downsampling keeps the endpoints\n");
  {
    std::vector<ucv::SeriesPoint> big;
    for (int i = 0; i < 1000; ++i) {
      ucv::SeriesPoint p;
      p.index = static_cast<std::uint32_t>(i);
      p.t_seconds = i * 0.01;
      big.push_back(p);
    }
    const auto small = ucv::downsample(big, 320);
    check("reduced to the limit", small.size() == 320,
          std::to_string(small.size()));
    check("first point preserved", small.front().index == 0);
    check("last point preserved", small.back().index == 999,
          std::to_string(small.back().index));
  }

  std::printf("\n[reader] a truncated tail is skipped, not fatal\n");
  {
    const auto partial = dir / "receiver-PARTIAL.ndjson";
    {
      std::ofstream out(partial, std::ios::binary | std::ios::trunc);
      out << R"({"type":"meta","run_id":"P2","protocol":"raw_udp","mode":"a","condition":"clean","clock_offset_ns":0,"clock_best_rtt_ns":0})" << "\n";
      out << R"({"type":"frame","seq":1,"cap_ns":0,"enc_ns":0,"snd_ns":0,"rcv_ns":0,"bytes":10,"key":true})" << "\n";
      out << R"({"type":"frame","seq":2,"cap_ns":0,"enc)";   // cut mid-line
    }
    const ucv::RunLog p = ucv::read_run_log(partial, dir);
    check("complete frames still parsed", p.frames.size() == 1,
          std::to_string(p.frames.size()));
    check("truncated line counted, not fatal", p.parse_errors == 1,
          std::to_string(p.parse_errors));
    check("no read error reported", !p.read_error.has_value());
    std::filesystem::remove(partial);
  }

  std::printf("\n[labels] comparison groups round-trip through the shared file\n");
  {
    ucv::LabelStore store(dir);
    check("unlabelled run has no group",
          store.group_for("receiver-PARITY.ndjson").empty());
    check("group saved", store.set_group("receiver-PARITY.ndjson", "Uji A 720p"));
    ucv::LabelStore reread(dir);
    check("group persisted and reloaded",
          reread.group_for("receiver-PARITY.ndjson") == "Uji A 720p",
          reread.group_for("receiver-PARITY.ndjson"));
    check("clearing removes the group",
          reread.set_group("receiver-PARITY.ndjson", "") &&
              ucv::LabelStore(dir).group_for("receiver-PARITY.ndjson").empty());
  }

  std::filesystem::remove(file);
  std::printf("\n%s (%d failures)\n",
              g_failures ? "FAILURES" : "ALL CHECKS PASSED", g_failures);
  return g_failures ? 1 : 0;
}
