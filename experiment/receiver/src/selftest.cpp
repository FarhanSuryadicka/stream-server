// Self-test for the measurement primitives.
//
// These are worth testing precisely because a bug here does not crash — it
// silently produces plausible-looking wrong numbers, which is the worst
// failure mode a measurement harness can have.
//
// Build: cmake --build . --target ucv-selftest && ./ucv-selftest

#include "ucv_wire.h"
#include "ucv_stats.h"
#include "ucv_remote_start.h"
#include "ucv_rtsp.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("  FAIL: %s\n", msg);                        \
      g_failures++;                                            \
    } else {                                                   \
      std::printf("  ok  : %s\n", msg);                        \
    }                                                          \
  } while (0)

// Underscored locals: an unadorned `d` here would shadow a caller's own `d`
// (e.g. a Distribution named d), which is a real bug this test hit.
#define CHECK_NEAR(a, b, tol, msg)                             \
  do {                                                         \
    const double ucv_got_  = (double)(a);                      \
    const double ucv_want_ = (double)(b);                      \
    const double ucv_diff_ = ucv_got_ - ucv_want_;             \
    if (ucv_diff_ > (tol) || ucv_diff_ < -(tol)) {             \
      std::printf("  FAIL: %s (got %.6f, want %.6f)\n", msg,   \
                  ucv_got_, ucv_want_);                        \
      g_failures++;                                            \
    } else {                                                   \
      std::printf("  ok  : %s\n", msg);                        \
    }                                                          \
  } while (0)

static void TestWireSizes() {
  std::printf("\n[wire] struct sizes are a contract with the spec\n");
  CHECK(sizeof(ucv_frame_header_t) == 56, "frame header is 56 bytes");
  CHECK(sizeof(ucv_control_t) == 32, "control message is 32 bytes");
  CHECK(sizeof(ucv_ack_t) == 40, "ack message is 40 bytes");

  // Field offsets must match the spec table exactly, or the Android sender
  // and this receiver will disagree about what a byte means.
  CHECK(offsetof(ucv_frame_header_t, frame_seq) == 8, "frame_seq at offset 8");
  CHECK(offsetof(ucv_frame_header_t, packet_seq) == 12, "packet_seq at 12");
  CHECK(offsetof(ucv_frame_header_t, payload_bytes) == 16, "payload_bytes at 16");
  CHECK(offsetof(ucv_frame_header_t, fragment_index) == 20, "fragment_index at 20");
  CHECK(offsetof(ucv_frame_header_t, fragment_count) == 22, "fragment_count at 22");
  CHECK(offsetof(ucv_frame_header_t, t_capture_ns) == 24, "t_capture_ns at 24");
  CHECK(offsetof(ucv_frame_header_t, t_encoded_ns) == 32, "t_encoded_ns at 32");
  CHECK(offsetof(ucv_frame_header_t, t_sent_ns) == 40, "t_sent_ns at 40");
  CHECK(offsetof(ucv_frame_header_t, run_id_hash) == 48, "run_id_hash at 48");
  CHECK(offsetof(ucv_frame_header_t, header_crc32) == 52, "header_crc32 at 52");
}

static void TestCrcRoundTrip() {
  std::printf("\n[crc] corrupt headers must be rejected, not repaired\n");
  ucv_frame_header_t h;
  std::memset(&h, 0, sizeof(h));
  h.frame_seq = 1234;
  h.payload_bytes = 45231;
  h.t_capture_ns = 111;
  h.t_encoded_ns = 222;
  h.t_sent_ns = 333;
  ucv_frame_header_finalize(&h);

  CHECK(ucv_frame_header_valid(&h), "valid header accepted");

  // Flip one bit in frame_seq — the field whose corruption would poison the
  // loss statistics worst of all.
  ucv_frame_header_t bad = h;
  bad.frame_seq ^= 0x40;
  CHECK(!ucv_frame_header_valid(&bad), "corrupt frame_seq rejected");

  ucv_frame_header_t wrong_magic = h;
  wrong_magic.magic = 0xDEADBEEF;
  CHECK(!ucv_frame_header_valid(&wrong_magic), "wrong magic rejected");

  ucv_frame_header_t wrong_ver = h;
  wrong_ver.version = 99;
  CHECK(!ucv_frame_header_valid(&wrong_ver), "wrong version rejected");

  ucv_control_t c;
  std::memset(&c, 0, sizeof(c));
  c.cmd_type = UCV_CMD_SET_ALPHA;
  c.cmd_seq  = 7;
  ucv_control_finalize(&c);
  CHECK(ucv_control_valid(&c), "valid control accepted");
  c.payload[0] ^= 0x01;
  CHECK(!ucv_control_valid(&c), "corrupt control payload rejected");

  ucv_ack_t a;
  std::memset(&a, 0, sizeof(a));
  a.cmd_seq = 7;
  ucv_ack_finalize(&a);
  CHECK(ucv_ack_valid(&a), "valid ack accepted");
  a.t_recv_ns ^= 0x100;
  CHECK(!ucv_ack_valid(&a), "corrupt ack rejected");
}

static void TestClockOffset() {
  std::printf("\n[clock] offset recovery on a symmetric path\n");
  // Phone clock runs 5,000,000 ns (5 ms) ahead of the PC. One-way delay
  // 2 ms each direction, phone takes 1 ms to turn the ACK around.
  const int64_t true_offset = 5'000'000;
  const uint64_t t1 = 1'000'000'000;
  const uint64_t t2 = static_cast<uint64_t>(static_cast<int64_t>(t1) + 2'000'000 + true_offset);
  const uint64_t t3 = t2 + 1'000'000;
  const uint64_t t4 = static_cast<uint64_t>(static_cast<int64_t>(t3) + 2'000'000 - true_offset);

  const int64_t est = ucv_clock_offset_ns(t1, t2, t3, t4);
  CHECK_NEAR(est, true_offset, 1000, "offset recovered on symmetric path");

  const int64_t rtt = ucv_round_trip_ns(t1, t2, t3, t4);
  CHECK_NEAR(rtt, 4'000'000, 1000, "round trip excludes phone turnaround");

  // Asymmetric path: 1 ms up, 3 ms down. This is the case the harness spec
  // warns about — the estimate is off by half the asymmetry, and the test
  // documents that limitation rather than hiding it.
  const uint64_t a2 = static_cast<uint64_t>(static_cast<int64_t>(t1) + 1'000'000 + true_offset);
  const uint64_t a3 = a2 + 1'000'000;
  const uint64_t a4 = static_cast<uint64_t>(static_cast<int64_t>(a3) + 3'000'000 - true_offset);
  const int64_t  a_est = ucv_clock_offset_ns(t1, a2, a3, a4);
  CHECK_NEAR(a_est, true_offset - 1'000'000, 1000,
             "asymmetric path skews offset by half the asymmetry (documented)");
}

static void TestPercentiles() {
  std::printf("\n[stats] percentiles — p99 is what we actually rank on\n");
  ucv::Distribution d;
  for (int i = 1; i <= 100; i++) d.Add(static_cast<double>(i));

  CHECK_NEAR(d.Percentile(0.50), 50.5, 0.01, "p50 of 1..100");
  CHECK_NEAR(d.Percentile(0.0), 1.0, 0.01, "p0 is the minimum");
  CHECK_NEAR(d.Percentile(1.0), 100.0, 0.01, "p100 is the maximum");
  CHECK_NEAR(d.Mean(), 50.5, 0.01, "mean of 1..100");
  CHECK_NEAR(d.Max(), 100.0, 0.01, "max");

  // Receiver live reporting asks for p50 every five seconds, then continues
  // appending samples. This exact sequence previously left the cached
  // `sorted_` flag true and produced impossible final output such as
  // p50 > p95 > p99.
  ucv::Distribution incremental;
  incremental.Add(30.0);
  incremental.Add(10.0);
  CHECK_NEAR(incremental.Percentile(0.50), 20.0, 0.01,
             "incremental distribution sorts its first batch");
  incremental.Add(40.0);
  incremental.Add(20.0);
  CHECK_NEAR(incremental.Percentile(0.50), 25.0, 0.01,
             "append after live percentile invalidates cached sort");
  CHECK_NEAR(incremental.Percentile(0.95), 38.5, 0.01,
             "tail percentile remains monotonic after append");
  CHECK_NEAR(incremental.Max(), 40.0, 0.01,
             "max remains correct after append");

  ucv::Distribution e;
  CHECK(e.empty(), "empty distribution reports empty");
  CHECK_NEAR(e.Percentile(0.5), 0.0, 0.001, "empty percentile is 0, not UB");

  // The scenario from the spec: a protocol whose mean looks fine but whose
  // tail does not. The harness must surface the difference.
  ucv::Distribution tail;
  for (int i = 0; i < 99; i++) tail.Add(40.0);
  tail.Add(400.0);
  CHECK_NEAR(tail.Percentile(0.50), 40.0, 0.01, "p50 unaffected by one outlier");
  CHECK(tail.Max() > 399.0, "max exposes the outlier the mean hides");
}

static void TestSequenceTracking() {
  std::printf("\n[stats] loss, reorder and duplicate accounting\n");
  {
    ucv::SequenceTracker s;
    for (uint32_t i = 0; i < 10; i++) s.Observe(i);
    CHECK(s.received() == 10, "10 contiguous frames counted");
    CHECK(s.gap_frames() == 0, "no gaps on a clean sequence");
    CHECK(s.reorder_events() == 0, "no reorder on a clean sequence");
  }
  {
    ucv::SequenceTracker s;
    s.Observe(0); s.Observe(1); s.Observe(5);  // 2,3,4 lost
    CHECK(s.gap_frames() == 3, "three missing frames detected");
    CHECK(s.gap_events() == 1, "counted as one gap event");
  }
  {
    ucv::SequenceTracker s;
    s.Observe(0); s.Observe(2); s.Observe(1);
    CHECK(s.reorder_events() == 1, "late frame counted as reorder");
  }
  {
    ucv::SequenceTracker s;
    s.Observe(0); s.Observe(1);
    const bool second = s.Observe(1);
    CHECK(!second, "duplicate rejected by return value");
    CHECK(s.duplicates() == 1, "duplicate counted");
    CHECK(s.received() == 2, "duplicate not double-counted as received");
  }
}

static void TestJitter() {
  std::printf("\n[stats] jitter must be immune to clock offset\n");
  // Perfectly paced stream, but the receiver's clock is 1 second off.
  // RFC 3550's difference-of-differences makes the offset cancel; if it did
  // not, jitter would be unusable whenever clock sync was imperfect.
  ucv::JitterEstimator j;
  const int64_t offset = 1'000'000'000;
  for (int i = 0; i < 50; i++) {
    const int64_t sent = static_cast<int64_t>(i) * 33'000'000;
    j.Update(sent, sent + offset);
  }
  CHECK_NEAR(j.jitter_ms(), 0.0, 0.001, "zero jitter despite 1 s clock offset");

  ucv::JitterEstimator j2;
  for (int i = 0; i < 200; i++) {
    const int64_t sent = static_cast<int64_t>(i) * 33'000'000;
    const int64_t wobble = (i % 2) ? 5'000'000 : 0;  // 5 ms alternating
    j2.Update(sent, sent + offset + wobble);
  }
  CHECK(j2.jitter_ms() > 1.0, "alternating 5 ms arrival wobble raises jitter");
}

static void TestRunIdHash() {
  std::printf("\n[wire] run_id hash separates concurrent runs\n");
  const uint32_t a = ucv_run_id_hash("20260811T140355Z-webrtc-01");
  const uint32_t b = ucv_run_id_hash("20260811T140355Z-webrtc-02");
  CHECK(a != b, "different run ids hash differently");
  CHECK(a == ucv_run_id_hash("20260811T140355Z-webrtc-01"), "hash is stable");
}

static void TestRemoteStartRouting() {
  std::printf("\n[control] dashboard protocols route to Android transports\n");
  CHECK(ucv_remote_start_supported(UCV_PROTO_RAWUDP), "Raw UDP remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_SRT), "SRT remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_MJPEG), "MJPEG remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_RTSP), "bare RTP remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_HLS), "HLS remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_RTMPS), "RTMP remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_WEBRTC), "WebRTC remote START enabled");
  CHECK(ucv_remote_start_supported(UCV_PROTO_RTSP_SIGNALLED),
        "signalled RTSP remote START enabled");
  CHECK(!ucv_remote_start_supported(999), "unknown protocol rejected");

  CHECK(ucv_remote_default_port(UCV_PROTO_RAWUDP) == UCV_PORT_RAWUDP,
        "Raw UDP default port");
  CHECK(ucv_remote_default_port(UCV_PROTO_SRT) == UCV_PORT_SRT,
        "SRT default port");
  CHECK(ucv_remote_default_port(UCV_PROTO_MJPEG) == UCV_PORT_MJPEG,
        "MJPEG default port");
  CHECK(ucv_remote_default_port(UCV_PROTO_RTSP) == UCV_PORT_RTP,
        "bare RTP default media port");
  CHECK(ucv_remote_default_port(UCV_PROTO_HLS) == UCV_PORT_HLS,
        "HLS default HTTP port");
  CHECK(ucv_remote_default_port(UCV_PROTO_RTMPS) == UCV_PORT_RTMPS,
        "RTMP default publish port");
  CHECK(ucv_remote_default_port(UCV_PROTO_WEBRTC) == UCV_PORT_SIGNAL,
        "WebRTC default signalling port");
  CHECK(ucv_remote_default_port(UCV_PROTO_RTSP_SIGNALLED) == UCV_PORT_RTSP,
        "signalled RTSP default control port");
  CHECK(ucv_remote_default_port(999) == 0, "unknown protocol has no port");
  CHECK(ucv_remote_config_is_port_only(UCV_PROTO_MJPEG),
        "MJPEG receives a bare listen port");
  CHECK(!ucv_remote_config_is_port_only(UCV_PROTO_HLS),
        "HLS accepts the common peer:port configuration");
}

static void TestRtspResponseParsing() {
  std::printf("\n[rtsp] response framing and case-insensitive headers\n");
  const std::string sdp =
      "v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n";
  const std::string raw =
      "RTSP/1.0 200 OK\r\n"
      "cseq: 3\r\n"
      "Session: A1B2C3D4;timeout=60\r\n"
      "Content-Length: " + std::to_string(sdp.size()) +
      "\r\nContent-Type: application/sdp\r\n\r\n" + sdp;
  ucv::RtspResponse response;
  CHECK(ucv::ParseRtspResponse(raw, &response), "complete response parsed");
  CHECK(response.status_code == 200, "status code parsed");
  CHECK(response.cseq == 3, "CSeq parsed case-insensitively");
  CHECK(response.Header("session") == "A1B2C3D4;timeout=60",
        "Session header parsed case-insensitively");
  CHECK(response.body == sdp, "Content-Length body preserved exactly");

  const std::string truncated = raw.substr(0, raw.size() - 1);
  CHECK(!ucv::ParseRtspResponse(truncated, &response),
        "truncated SDP body rejected");
  CHECK(!ucv::ParseRtspResponse("HTTP/1.1 200 OK\r\nCSeq: 1\r\n\r\n",
                                &response),
        "non-RTSP status line rejected");
}

static void TestRtspClientSession() {
  std::printf("\n[rtsp] OPTIONS -> DESCRIBE -> SETUP -> PLAY -> TEARDOWN\n");
  CHECK(ucv::NetInit(), "network runtime initialised");

  ucv::TcpServer server;
  int port = 0;
  for (int candidate = 38554; candidate < 38564 && !port; candidate++) {
    if (server.Listen(candidate)) port = candidate;
  }
  CHECK(port != 0, "mock RTSP server listening");
  if (!port) {
    ucv::NetShutdown();
    return;
  }

  std::vector<std::string> requests;
  bool server_ok = false;
  std::thread server_thread([&] {
    if (!server.AcceptTimeout(3000)) return;
    const std::string sdp =
        "v=0\r\n"
        "m=video 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:track0\r\n";
    for (int index = 0; index < 5; index++) {
      std::string request;
      char buf[1024];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const int n = server.RecvTimeout(buf, sizeof(buf), 1500);
        if (n <= 0) return;
        request.append(buf, static_cast<size_t>(n));
        if (request.size() > 8192) return;
      }
      requests.push_back(request);

      const int cseq = index + 1;
      std::string extra;
      std::string body;
      if (index == 0) {
        extra = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n";
      } else if (index == 1) {
        extra = "Content-Type: application/sdp\r\n";
        body = sdp;
      } else if (index == 2) {
        extra =
            "Transport: RTP/AVP;unicast;client_port=35004-35005;"
            "server_port=5004-5005\r\n"
            "Session: ABCD1234;timeout=60\r\n";
      } else {
        extra = "Session: ABCD1234\r\n";
      }
      std::string response =
          "RTSP/1.0 200 OK\r\nCSeq: " + std::to_string(cseq) + "\r\n" +
          extra;
      if (!body.empty())
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
      response += "\r\n" + body;
      if (!server.SendAll(response.data(), response.size())) return;
    }
    server_ok = true;
  });

  ucv::RtspClient client;
  const bool started = client.Start("127.0.0.1", port, 35004, 2000);
  const bool active = client.active();
  const std::string session = client.session();
  const bool torn_down = started && client.Teardown(2000);
  server_thread.join();

  CHECK(started, "RTSP client completed PLAY");
  CHECK(active, "session marked active after PLAY");
  CHECK(session == "ABCD1234", "Session timeout parameter stripped");
  CHECK(torn_down, "TEARDOWN acknowledged");
  CHECK(server_ok && requests.size() == 5, "all five RTSP methods observed");
  if (requests.size() == 5) {
    CHECK(requests[0].find("OPTIONS rtsp://") == 0, "OPTIONS sent first");
    CHECK(requests[1].find("DESCRIBE rtsp://") == 0, "DESCRIBE sent second");
    CHECK(requests[2].find("SETUP rtsp://") == 0, "SETUP sent third");
    CHECK(requests[2].find("client_port=35004-35005") != std::string::npos,
          "SETUP negotiates the bound RTP/RTCP ports");
    CHECK(requests[3].find("PLAY rtsp://") == 0, "PLAY sent fourth");
    CHECK(requests[3].find("Session: ABCD1234") != std::string::npos,
          "PLAY carries negotiated Session");
    CHECK(requests[4].find("TEARDOWN rtsp://") == 0, "TEARDOWN sent last");
  }
  server.Close();
  ucv::NetShutdown();
}

int main() {
  std::printf("ucv measurement primitives — self test\n");
  std::printf("======================================\n");

  TestWireSizes();
  TestCrcRoundTrip();
  TestClockOffset();
  TestPercentiles();
  TestSequenceTracking();
  TestJitter();
  TestRunIdHash();
  TestRemoteStartRouting();
  TestRtspResponseParsing();
  TestRtspClientSession();

  std::printf("\n======================================\n");
  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
