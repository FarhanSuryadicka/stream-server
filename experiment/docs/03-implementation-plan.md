# Implementation Plan — Seven Protocols

Honest status and the work each protocol still needs. Nothing here is claimed
to be measured; the "Status" column says exactly what exists.

---

## 0. Where things actually stand

| # | Protocol | Sender (Android) | Receiver (PC) | Can produce numbers? |
|---|---|---|---|---|
| 4 | Raw UDP | ✅ implemented, compiled | ✅ implemented, tested E2E | ✅ **yes — ready to measure on-device** |
| 5 | MJPEG/HTTP | ✅ camera JPEG passthrough, compiled | ✅ HTTP multipart reader, compiled | ⚠️ needs on-device validation |
| 2 | SRT | ⛔ stub | ⛔ | ❌ needs libsrt |
| 3 | RTSP/RTP | ⛔ stub | ⛔ | ❌ needs RTP stack |
| 1 | WebRTC | ⛔ stub | ⛔ | ❌ needs libdatachannel |
| 6 | RTMPS | ⛔ stub | ⛔ | ❌ needs librtmp + server |
| 7 | HLS/DASH | ⛔ stub | ⛔ | ❌ needs segmenter |

**The H.264 encoder is now built** (§1) — MediaCodec via the NDK, feeding the
shared pipeline. Raw UDP can therefore be measured end-to-end as soon as a
phone and camera are attached.

Also done, and the part that is expensive to get wrong: the harness, the wire
contract, the instrumentation, the clock sync, the metric maths, and a
receiver — verified end-to-end against a mock sender (§6).

> **Not yet run on real hardware.** Everything below compiles, links, and its
> logic is unit-tested, but no frame has passed through a physical camera and
> a real encoder yet. Treat the first on-device run as a bring-up, not as a
> measurement (§11).

---

## 1. H.264 encoder — BUILT

Route A was taken: **MediaCodec via the NDK (`AMediaCodec`), entirely in C**,
so no JNI round-trip per frame and all timestamps stay on one clock.

```
UVC MJPEG frame ──▶ libjpeg-turbo ──▶ NV12 ──▶ MediaCodec H.264 ──▶ transport
                    (JCS_YCbCr out)                                    │
  ucv_pipeline.c capture thread          ucv_pipeline.c drain thread ──┘
```

Files: [`ucv_encoder.c`](../../app/src/main/cpp/ucv_encoder.c),
[`ucv_pipeline.c`](../../app/src/main/cpp/ucv_pipeline.c).

### Decisions worth knowing

**Decode goes straight to YCbCr, not via RGB.** libjpeg is asked for
`JCS_YCbCr` output, so there is one colour conversion per frame instead of two.

**Two threads, not one.** MediaCodec's input and output sides run at different
rates; draining inline would block capture whenever the encoder was busy, and
that would distort the frame pacing every jitter and latency figure is measured
against.

**Capture timestamps travel through a PTS ring.** MediaCodec only carries a
presentation timestamp, so `t_capture` is stashed keyed by PTS and recovered on
the way out. If an entry is evicted (encoder fell badly behind), the frame is
**dropped and counted** rather than sent with a wrong `t_capture` — a wrong
timestamp silently corrupts the glass-to-glass numbers, a dropped frame does
not.

**`frame_seq` is assigned at send time, not capture time.** So a gap the
receiver sees is unambiguously network loss, never a local encoder drop. Those
are two very different findings and must not be conflated.

**Software encoders are rejected outright.** `c2.android.*` / `OMX.google.*`
cause `ucv_encoder_create()` to fail rather than warn, because a software run
changes the encode cost baked into every latency number.

### Known caveat — API level

`AMediaCodec_getName` is API 28+, but `minSdk` is 24. It is resolved via
`dlsym`, so on API 24–27 the hardware check reports **"undetermined"** rather
than passing silently. Profile/level/bitrate-mode are set by string key so they
still reach the codec below 28.

> On a device below API 28, `hardware_accelerated` cannot be verified. Record
> that against the run rather than assuming it passed.

### Verified so far

- Compiles and links; `AMediaCodec_*` symbols resolve against `libmediandk.so`
- JPEG→NV12 conversion unit-tested on the PC (`ucv-nv12test`): plane geometry,
  luma indexing, 2×2 chroma subsampling, buffer bounds
- **A real bug was caught this way**: the chroma plane was being written
  V-then-U (NV21) where `COLOR_FormatYUV420SemiPlanar` requires U-then-V
  (NV12). That mistake encodes and transmits perfectly and merely swaps the
  colours, so it would have looked like a camera fault on the rig.

### Not verified

No frame has yet passed through a real camera and a real hardware encoder.
See §11 for what to check on first bring-up.

---

## 2. Raw UDP — protocol #4

**Status: done and tested.** `ucv_transport.c`, compiled into the `.so`
(symbol `ucv_transport_rawudp` verified present).

Application-layer fragmentation at 1200 B, full preamble per fragment,
`t_sent_ns` stamped immediately before `sendto`. Receiver side implemented and
exercised end-to-end including a synthetic-loss run that the loss accounting
correctly reported.

Remaining: nothing, beyond feeding it real encoded frames from §1.

---

## 3. MJPEG over HTTP — protocol #5

**Status: implemented on both sides; on-device validation remains.** Android
forwards the camera's JPEG bytes directly (no MediaCodec step), while the PC
reader parses each multipart `Content-Length` body and its `X-UCV-*` timing
headers. The dashboard can select `mjpeg`, graph the live run, and display the
JPEG preview without FFmpeg.

Instrumentation headers per `02-wire-format.md` §1.2, `TCP_NODELAY` set (Nagle
would otherwise add tens of ms that belong to the socket option, not the
protocol, and would unfairly penalise MJPEG).

**Remaining work:** validate a physical-camera run. For TCP, sequence gaps
measure application-visible frame loss; IP retransmissions require a packet
capture and cannot be inferred from HTTP.

> Note for the results table: MJPEG will be measured carrying **JPEG**, not
> H.264 — it has no inter-frame compression by definition. Its bandwidth
> numbers therefore are not comparable to the others, and the report must say
> so rather than listing them in the same column without comment. Its latency
> and control-channel numbers remain comparable.

---

## 4. SRT — protocol #2

**Library:** [libsrt](https://github.com/Haivision/srt) — vendor into
`app/src/main/cpp/srt/`, same pattern as libuvc.

Notes that will cost time if discovered late:
- libsrt needs a crypto backend for AES. Bundle **mbedTLS** (smallest NDK
  footprint) rather than OpenSSL.
- Configure `SRTO_LATENCY` explicitly and **log it** — it is the single
  biggest determinant of SRT's measured latency, and leaving it at default
  while another protocol is tuned is exactly the unfair comparison §2 of the
  harness spec warns about. Measure at 20/50/120 ms and report all three.
- Caller/listener: phone = caller, PC = listener.
- PC side: `srt-live-transmit`, or link libsrt into the receiver.

**Estimate: 3–5 days** including cross-compiling mbedTLS for arm64.

---

## 5. RTSP / RTP over UDP — protocol #3

Two parts: an RTSP control server (TCP, `DESCRIBE`/`SETUP`/`PLAY`) and RTP
packetisation (RFC 6184 for H.264).

- Instrumentation rides in an **RTP header extension** (RFC 8285), not the
  binary preamble — otherwise the stream is not real RTSP and any player
  comparison is meaningless.
- FU-A fragmentation for NALs over MTU.
- PC side: ffmpeg/GStreamer can validate interoperability, but the measuring
  receiver must read the extension, so a small custom RTP reader is needed.

**Estimate: 5–7 days.** Consider vendoring a small RTSP library instead of
hand-rolling.

---

## 6. WebRTC — protocol #1

Most work, and most likely to win — it is the only protocol with a genuine
native low-latency back-channel (`RTCDataChannel`), which is precisely the
property the issue says decides the winner.

- **Library:** [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
  — far smaller than Google's `libwebrtc` and sufficient here.
- Needs a signalling server (WebSocket) for SDP/ICE. Trivial locally, but it
  is **infrastructure the other protocols do not need** — record that in the
  operational-complexity column, it is a real deployment cost.
- Feed the H.264 encoder output as an external track; do **not** let the
  library re-encode, or the frozen config is violated.
- Measure the control channel **twice**: over the native data channel, and
  over the shared UDP control socket. That is what makes the native-vs-paired
  comparison in harness spec §7.3 honest rather than assumed.

**Estimate: 7–10 days.**

---

## 7. RTMPS — protocol #6

Deliberately included as the high-latency contrast. Expect 2–5 s.

- Needs a media server (nginx-rtmp / SRS / MediaMTX) — a third box in the path,
  which is itself part of the finding.
- Instrumentation via AMF metadata per frame.
- **Estimate: 2–3 days**, mostly server setup.

> Prediction to verify, not assume: TCP head-of-line blocking should make p99
> far worse than p50 here. If the measurement disagrees, the measurement wins.

---

## 8. HLS / DASH — protocol #7

The other high-latency contrast (5–30 s). Segmenter + HTTP server.

- Instrumentation via `EXT-X-PROGRAM-DATE-TIME` plus a per-segment sidecar
  JSON, since there is no per-frame carriage.
- Report **segment duration** prominently — it dominates the latency and is
  the only real tuning knob.
- **Estimate: 2–3 days.**

---

## 9. Suggested order

Ordered by what de-risks the experiment earliest, not by protocol number:

1. **H.264 encoder** (§1) — unblocks everything. *2–4 d*
2. **Raw UDP end-to-end with real frames** — proves the whole chain on real
   video; the transport itself is already done. *0.5 d*
3. **MJPEG receiver** (§3) — completes the existing baseline. *1 d*
4. **SRT** (§4) — first genuinely interesting result. *3–5 d*
5. **WebRTC** (§6) — the likely winner; worth knowing early. *7–10 d*
6. **RTSP/RTP** (§5) — *5–7 d*
7. **RTMPS** (§7) + **HLS** (§8) — contrast cases, cheapest last. *4–6 d*

**Total: roughly 5–7 weeks of focused work.** Steps 1–3 (about a week) already
produce a publishable partial result: Raw UDP and MJPEG measured properly, with
a harness the remaining protocols slot into unchanged.

---

## 11. First on-device bring-up

The encoder path compiles and its logic is unit-tested, but it has never seen a
real camera or a real hardware encoder. **The first run is bring-up, not
measurement** — do not record its numbers.

### Procedure

1. Phone: **Open USB Camera** → grant permissions
2. Phone: enter the PC's IP → **1. Start Control**
3. PC: `ucv-receiver --phone <phone-ip> --run-id <same-id> --duration 60`
4. Phone: **2. Start Raw UDP**

The on-screen panel shows the negotiated encoder and live counters
(`cap / dec / enc / sent`, decode and encode times, error counts).

### What to check, and what it means if it is wrong

| Symptom | Likely cause |
|---|---|
| `encoder init failed` | No hardware H.264 encoder, or one rejected as software. Check the logged codec name. |
| `hw=NO` | Software encoder selected — the run is invalid by §2 of the harness spec. |
| `hw=undetermined` | Device is below API 28. Record it against the run; do not assume it passed. |
| `decerr` climbing | JPEG decode failing. Check the camera's actual mode matches what was negotiated. |
| `cap` rises, `dec` flat | Every frame failing to decode — usually a resolution mismatch. |
| `dec` rises, `enc` flat | Encoder accepting input but producing nothing. Check colour format. |
| `enc` rises, `sent` flat | Transport not connected. Check the PC IP and that the receiver is listening. |
| `drop` climbing steadily | Encoder cannot keep up. Lower resolution/fps before drawing any conclusion about a protocol. |
| Colours look wrong | Chroma order. The NV12/NV21 bug this codebase already hit once — re-run `ucv-nv12test`. |
| Receiver reports frames but garbled video | SPS/PPS not reaching the decoder. Codec config is prepended to the first keyframe; check it is present. |

### Sanity checks before trusting the first real run

- `cap ≈ dec ≈ enc ≈ sent`, all climbing at ~30/s
- `decerr`, `drop`, `senderr` all at or near zero
- `encode` time well under the frame interval (33 ms at 30 fps)
- Receiver's `capture→encode p50` is plausible for the device (single-digit to
  low-tens of ms on hardware)
- Receiver reports `clock_status: OK`, not `CLOCK_SUSPECT`

---

## 10. Guardrails already built in

These are in the code, not just in this document:

- **`ucv_transport_get()` returns NULL for unimplemented protocols.** No silent
  fallback, so a run can never be labelled `srt` while its bytes actually went
  over plain UDP.
- **The receiver aborts if clock sync fails.** A run without sync would report
  latency numbers that are pure clock offset. Verified: it refuses.
- **`run_id_hash` in every frame.** A stale sender from a previous run is
  counted and discarded, not averaged in.
- **Header CRC.** A corrupt `frame_seq` would poison loss statistics worse than
  a dropped frame; such frames are rejected.
- **Struct sizes are `static_assert`ed** against the spec — a padding change
  breaks the build instead of silently corrupting every measurement.
- **Wire header is one file shared by both sides**, not two copies that can
  drift.
- **Warmup discarded, `CLOCK_SUSPECT` flagged, tail-loss reconciled against the
  sender's own count** rather than inferred from sequence gaps alone.
