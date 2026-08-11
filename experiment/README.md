# Data Transfer Protocol Experiment

Measurement harness for the protocol comparison requested in the 20260810
issue. Answers **which transport fits our two-way architecture** with measured
evidence rather than specification reading.

> **Current operator guide:** [`RUNBOOK-CURRENT.md`](RUNBOOK-CURRENT.md).
> It documents the website-controlled camera agent, the active `.137.x` rig,
> wireless installation, live dashboard, and current troubleshooting. Older
> checklist/procedure files still describe the legacy manual workflow.

```
  Android phone                 Router               PC (C++ receiver)
  ─────────────                 ──────               ─────────────────
  UVC fisheye camera
    → H.264 encode  ══ video, protocol under test ══▶ measure + log
    ◀───────────── control (alpha/beta/zoom/preset) ── drive + measure RTT
```

---

## At the rig

**[`CHECKLIST.md`](CHECKLIST.md)** — one page, start here for every run.

## Read these in order

| Document | What it settles |
|---|---|
| [`docs/01-harness-spec.md`](docs/01-harness-spec.md) | Frozen encoder config, metric formulas, clock sync, run rules. **Freeze this before measuring anything.** |
| [`docs/02-wire-format.md`](docs/02-wire-format.md) | Binary contract between phone and PC |
| [`docs/03-implementation-plan.md`](docs/03-implementation-plan.md) | Honest per-protocol status and remaining work |
| [`docs/04-run-procedure.md`](docs/04-run-procedure.md) | Rig checklist, experiment matrix, how to read results |

## Local web dashboard

Build the receiver once, then start the dependency-free dashboard:

```powershell
.\experiment\build-receiver.ps1
.\experiment\run-dashboard.ps1
```

Open `http://127.0.0.1:8088/`. The dashboard can launch the receiver, show its
console output and decoded camera preview live, graph per-frame
transport/glass/encode latency from raw
NDJSON, label older logs, and compare valid runs by capture mode and network
condition. New dashboard runs use the fixed `clean` condition label; advanced
network-condition controls are intentionally hidden until traffic shaping is
automated. On the phone, only open the USB camera; the app automatically starts
its camera-control agent. In the dashboard, click **Refresh camera modes**, pick
one of the MJPEG resolutions actually advertised by the camera, then click
**Start measurement**. FPS is selected automatically as the highest value the
camera advertises for that resolution. The receiver binds UDP first and
remotely starts/stops the phone
pipeline, and the dashboard generates the `run_id` automatically.

The live picture requires FFmpeg (`ffmpeg.exe`) on `PATH`, or at
`C:\Tools\ffmpeg\bin\ffmpeg.exe`. The receiver remains the only listener on
UDP 8201; it mirrors complete H.264 frames to a best-effort localhost preview
channel after reception, so the browser does not compete for phone packets.

New receiver logs accept `--mode 320x240@20` and `--condition clean` metadata.
The mode dropdown is populated from the opened camera over the control channel;
only one UVC mode runs at a time, preventing concurrent pipelines from
competing for isochronous USB bandwidth.

## Scripts

| Script | Purpose |
|---|---|
| `build-receiver.ps1` | Build the PC receiver + run its tests |
| `run-dashboard.ps1` | Launch the local measurement and analysis dashboard |
| `install-phone.ps1` | Build and install the Android app (`-Clean` after native edits) |
| `Get-RigInfo.ps1` | Which PC IP to enter on the phone |
| `Test-Connectivity.ps1` | Pre-flight: subnet, reachability, firewall |
| `Measure-Baseline.ps1` | Network baseline — required to interpret control RTT |

---

## What exists right now

**Built and verified:**

- **H.264 encoder pipeline** — UVC capture → libjpeg-turbo decode → NV12 →
  MediaCodec (NDK, native C) → transport. Software encoders are rejected, not
  warned about.
- Wire format shared by both sides — one header file, `static_assert`ed to
  56/32/40 bytes so a padding change breaks the build instead of silently
  corrupting measurements
- Clock synchronisation (SNTP-style, 64 probes, lowest-RTT wins)
- Android: Raw UDP, RFC 6184 RTP/UDP, instrumented MJPEG transport, control
  channel, encoder, and shared pipeline
- PC: `ucv-receiver` — Raw UDP/RTP receive and reassembly, packet loss,
  MJPEG/TCP retransmission-rate, jitter accounting, control-RTT-under-load,
  NDJSON logging
- `ucv-selftest` (34 checks) and `ucv-nv12test` — both passing
- `ucv-mocksender` — phone stand-in; the whole path was validated end-to-end
  on one machine, including synthetic loss the receiver correctly reported
- `analysis/analyze.py` — turns logs into the comparison table

**Not built yet:**

- SRT, WebRTC, RTMPS, and HLS transports (stubs return NULL, deliberately)
- RTSP signalling for the implemented RTP/UDP data plane

### Status: ready for on-device bring-up

**Raw UDP is measurable end-to-end** once a phone and camera are attached.

> Nothing here has run on real hardware yet. Everything compiles, links, and
> its logic is unit-tested — but no frame has passed through a physical camera
> and a real encoder. Treat the first on-device run as bring-up, not as a
> measurement. `docs/03-implementation-plan.md` §11 lists what to check and
> what each failure mode means.

The unit tests already earned their keep: they caught a chroma-ordering bug
(NV21 written where NV12 was required) that would have encoded and transmitted
perfectly and merely come out with swapped colours — the kind of fault that
gets blamed on the camera.

---

## Quick start

```bash
# 1. Verify the measurement primitives (do this before trusting any run)
cd experiment/receiver
g++ -std=c++17 -O2 -Iinclude src/selftest.cpp -o ucv-selftest && ./ucv-selftest
#    expect: ALL CHECKS PASSED

# 2. Build the receiver
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release

# 3. Exercise the whole path without a phone
./ucv-mocksender --run-id TEST-01 --duration 30 --drop-permil 20 &
./ucv-receiver --phone 127.0.0.1 --run-id TEST-01 --duration 25 --warmup 3

# 4. Analyse
python ../analysis/analyze.py receiver-TEST-01.ndjson
```

Windows: MinGW-w64 g++ works (`-lws2_32 -static`); MSVC should too but has not
been exercised.

> Mock-sender numbers describe the loopback path, not a protocol. They must
> never enter the results table.

---

## Design decisions worth knowing

**Only the transport varies.** Capture, encode and instrumentation live above
the `ucv_transport_t` interface, so a protocol cannot accidentally be measured
with a different encoder config — the failure mode the issue explicitly warns
about.

**Unimplemented protocols return NULL, never a fallback.** A run labelled `srt`
whose bytes actually went over plain UDP is the most damaging thing that could
happen to these results.

**The receiver aborts when clock sync fails.** Without an offset estimate,
`t_received - t_sent` measures clock skew, not latency. Verified: it refuses to
run.

**Logs hold raw timestamps only.** Every latency is derived at analysis time,
so a corrected formula can be re-applied to old runs without re-booking the rig.

**Control RTT is measured under video load,** on the PC clock alone. It is the
only metric that inherits no clock-sync error — and per the issue, the one that
decides whether a protocol is usable at all.

---

## Known limitations — state these in any report

- **One-way latency carries ±1–3 ms residual error.** The clock-sync method
  assumes a symmetric path; WiFi is not perfectly symmetric. Differences below
  ~5 ms between protocols are **not resolvable** by this harness. Sub-ms
  precision would need PTP hardware timestamping.
- **Loss semantics follow the transport.** Raw UDP uses datagram sequence gaps;
  MJPEG/TCP uses Android kernel retransmission counters. Frame gaps remain a
  fallback only when transport counters are unavailable.
- **Application-byte overhead is not wire overhead.** Retransmissions and
  header cost need a packet capture.
- **UDP fragment indexing is explicit.** Packet sequence and fragment index/count
  keep loss, reorder, duplicates, and reassembly failures as separate metrics.
- **MJPEG carries JPEG, not H.264.** Its bandwidth numbers are not comparable
  to the other six by construction; its latency and control numbers are.
