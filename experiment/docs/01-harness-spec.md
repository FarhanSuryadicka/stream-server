# Harness Specification — Data Transfer Protocol Experiment

> Status: **draft for team sign-off**. Nothing below is measured yet.
> Nobody runs a protocol measurement until this document is agreed and frozen.

Related issue: protocol experiment phase (20260810).

---

## 0. Why this document exists first

The issue says it plainly:

> If protocol A is measured at a different bitrate, resolution, or encoder preset
> than protocol B, the comparison is worthless and we will have to redo everything.

Every number this experiment produces is only meaningful *relative to the other
protocols measured under identical conditions*. So the harness — not the
protocols — is the thing that has to be right first.

This document defines:

1. The **frozen encoder configuration** (§2)
2. The **instrumentation contract** — what every protocol must timestamp (§3)
3. The **clock synchronisation** method, without which one-way latency is
   meaningless (§4)
4. The **metric definitions** — exactly what we mean by "latency" (§5)
5. The **run protocol** — what is logged for every single run (§6)
6. The **two-way requirement** — upstream control channel (§7)

---

## 1. System under test

```
  ┌────────────────────┐        ┌──────────┐        ┌────────────────────┐
  │  Android phone     │        │  Router  │        │  PC (C++ desktop)  │
  │                    │        │          │        │                    │
  │  UVC fisheye cam   │        │  802.11  │        │  receiver + decode │
  │   → MJPEG frame    │───────▶│  2.4/5G  │───────▶│   → display        │
  │   → decode (RGB)   │        │          │        │                    │
  │   → H.264 encode   │        │          │        │                    │
  │   → TRANSPORT ─────┼────────┼──────────┼────────┼──▶                 │
  │                    │◀───────┼──────────┼────────┼── control commands │
  └────────────────────┘        └──────────┘        └────────────────────┘
        DOWNSTREAM: video                                UPSTREAM: control
```

**The variable under test is the TRANSPORT layer only.** Everything upstream of
it (camera, decode, encode) and downstream of it (decode, display) must be
byte-for-byte identical across all seven protocols.

### Scope note — what the current app is and is not

The app in this repo today is an **MJPEG passthrough server**: it forwards raw
camera JPEG bytes with no encoder in the pipeline. That makes it a valid
implementation of protocol #5 *only*, and even then it cannot honour §2's frozen
bitrate because the bitrate is decided by the camera firmware.

Therefore: the H.264 encode stage (§2) is **new work** that must be added before
any protocol other than #5 can be measured. See `03-implementation-plan.md`.

---

## 2. Frozen encoder configuration

These values are **locked for the entire experiment**. Changing any of them
invalidates every run recorded before the change.

| Parameter | Value | Rationale |
|---|---|---|
| Capture resolution | `1280x720` | Known-good over isochronous USB on this rig; higher res stalls (see CLAUDE.md) |
| Capture frame rate | `30 fps` | Camera-offered discrete mode, no interpolation |
| Capture format | `MJPEG` | Only format the UVC layer enumerates |
| Encode codec | `H.264 (AVC)` | Issue specifies H.264 downstream |
| Encoder | `MediaCodec`, hardware | `OMX.*` / `c2.*` hardware encoder; **software fallback disqualifies the run** |
| Profile / Level | `Baseline / 3.1` | Widest decoder compatibility incl. WebRTC |
| Target bitrate | `4_000_000` bps (4 Mbps) | Fits 720p30 comfortably; leaves WiFi headroom |
| Bitrate mode | `CBR` | Removes VBR burst as a confounding variable |
| Keyframe interval | `1 second` (i.e. every 30 frames) | Short GOP so packet loss recovery is comparable |
| B-frames | `0` | Baseline profile; also avoids reorder latency |
| Colour format | `COLOR_FormatYUV420SemiPlanar` (NV12) | Standard MediaCodec input |

### Enforcement

The encoder configuration is **not** a per-protocol setting. It lives in one
place and is reported in every run's metadata block. The receiver validates the
reported config against the frozen values and **marks the run INVALID** if they
differ. See §6.

Any deviation must be recorded as a separate, explicitly-labelled experiment —
never mixed into the main comparison table.

---

## 3. Instrumentation contract

Every protocol must carry the same per-frame metadata, regardless of transport.
This is what makes the comparison possible at all.

### 3.1 Required per-frame fields

| Field | Type | Meaning |
|---|---|---|
| `frame_seq` | `uint32` | Monotonic, starts at 0, **never reset** during a run. Gaps = loss. |
| `t_capture` | `uint64` | ns, phone `CLOCK_MONOTONIC`, at the instant the frame left the camera |
| `t_encoded` | `uint64` | ns, phone clock, when the encoder emitted the last byte of this frame |
| `t_sent` | `uint64` | ns, phone clock, immediately before handing bytes to the transport |
| `is_keyframe` | `uint8` | 1 = IDR |
| `payload_bytes` | `uint32` | Encoded frame size, excluding our metadata |

The receiver adds, on arrival:

| Field | Meaning |
|---|---|
| `t_received` | ns, PC clock, when the last byte of the frame arrived |
| `t_decoded` | ns, PC clock, when the decoder output the frame |

### 3.2 How the metadata travels

Metadata must **not** be inferred or reconstructed — it is carried explicitly.
Transport-specific carriage:

| Protocol | Carriage mechanism |
|---|---|
| MJPEG/HTTP | Custom headers in each multipart part (`X-Seq`, `X-Cap-Ts`, …) |
| Raw UDP | 32-byte binary preamble before each datagram payload |
| SRT | Same 32-byte preamble, inside the SRT payload |
| RTP/RTSP | RTP header extension (RFC 8285) |
| WebRTC | RTP header extension, same as above |
| RTMPS | AMF metadata message per frame |
| HLS/DASH | `EXT-X-PROGRAM-DATE-TIME` + per-segment sidecar JSON |

The binary preamble format is defined in `02-wire-format.md` and is the default
for anything that can carry opaque bytes.

### 3.3 Non-negotiable rule

> Metadata overhead is itself measured and reported. A protocol is **not**
> allowed to skip instrumentation to look faster.

---

## 4. Clock synchronisation

**Without this section, every one-way latency number in this experiment is
fiction.** Phone and PC clocks are independent; naive `t_received - t_sent`
measures clock offset, not latency.

### 4.1 Method: SNTP-style offset estimation over the control channel

Before and after every run, the receiver performs a timing exchange:

```
PC  ──── PING(t1_pc) ────────────▶ Phone
                                   records t2_phone on arrival
PC  ◀─── PONG(t1_pc,t2_phone,t3_phone) ── Phone
    records t4_pc on arrival

round_trip = (t4_pc - t1_pc) - (t3_phone - t2_phone)
offset     = ((t2_phone - t1_pc) + (t3_phone - t4_pc)) / 2
```

- Run the exchange **64 times**, back to back.
- Keep the sample with the **lowest `round_trip`** — it has the least queueing
  noise and therefore the most trustworthy offset.
- Record `offset_before` and `offset_after` (start and end of run).
- **Drift check:** if `|offset_after - offset_before| > 2 ms`, mark the run
  `CLOCK_SUSPECT`. Do not silently average it away.

### 4.2 Honest limitation — state this in the report

This method assumes the network path is **symmetric** (uplink delay ≈ downlink
delay). Over WiFi that assumption is imperfect. Residual error is roughly
±(asymmetry/2), typically **1–3 ms** on a quiet LAN.

Consequences to accept and to write into the final report:

- Differences **below ~5 ms** between two protocols are **not resolvable** by
  this harness. Do not rank protocols on a 2 ms gap.
- **Round-trip** metrics (§5.4) do not depend on clock sync and are therefore
  more trustworthy. Prefer them when the gap is small.

If sub-millisecond one-way precision is ever needed, that requires PTP hardware
timestamping — out of scope here, and it should be stated as a limitation rather
than papered over.

---

## 5. Metric definitions

Ambiguity here is how teams end up arguing about numbers that measured different
things. Each metric below is defined by its exact formula.

### 5.1 Transport latency (the headline number)

```
transport_latency = (t_received + clock_offset) - t_sent
```

Pure network + protocol cost. **This is the metric that ranks the protocols**,
because it excludes camera and encoder, which are identical across runs anyway.

### 5.2 Glass-to-glass latency

```
glass_to_glass = (t_decoded + clock_offset) - t_capture
```

What a human actually perceives. Includes capture, encode, transport, decode.
Useful for the product decision, but a poor protocol discriminator — the
constant encode/decode cost dominates and compresses the differences.

### 5.3 Component breakdown

```
capture_to_encode = t_encoded - t_capture      (phone-local, no clock sync needed)
encode_to_send    = t_sent    - t_encoded      (phone-local)
transport         = §5.1
decode            = t_decoded - t_received     (PC-local, no clock sync needed)
```

Report these separately. **This is what stops us from blaming the protocol for
an encoder problem** — the single most common way this kind of experiment goes
wrong.

### 5.4 Control round-trip (upstream — see §7)

```
control_rtt = t_ack_received_pc - t_command_sent_pc
```

Measured entirely on the PC clock, so **immune to clock-sync error**. This is
the most reliable number in the whole experiment. Weight it accordingly.

#### 5.4.1 Network baseline — record this every session

Control RTT is only interpretable against the bare network's own latency.
Before each session, take an ICMP baseline over the same path:

```powershell
1..40 | % { (Test-Connection 192.168.0.101 -Count 1).ResponseTime }
```

Record its p50 / p95 / max in the run metadata.

#### 5.4.2 Observed on this rig — traffic density changes the path

Measured 2026-08-11 against the phone over WiFi → router → wired PC, with no
video running:

| Probe pattern | p50 | p95 | max |
|---|---|---|---|
| Sparse (~10–20/s, our control rate) | 9.2 ms | 145 ms | 319 ms |
| Back-to-back ICMP burst | 2 ms | 4 ms | 616 ms* |

\* single outlier on the first probe — the radio waking up.

**The same network gives a 36× worse p95 when traffic is sparse.** This is
Android WiFi power-save: dense traffic keeps the radio awake, sparse traffic
lets it sleep between packets, and each sparse datagram then waits for a
wake-up.

Two consequences that shape the whole experiment:

1. **Control RTT measured on an idle link is not the deployment case.** With
   video flowing at 30 fps the radio never sleeps, so the control channel
   should behave far better than the sparse figures above. This is exactly why
   §7.2 requires control RTT to be measured *under video load* — it is a
   correctness requirement, not a formality.

2. **A tail seen in a protocol run may belong to the radio, not the protocol.**
   Always record the baseline (§5.4.1) in the same session, and report control
   RTT as `measured − baseline` alongside the raw figure. Never rank protocols
   on a tail the network was already producing on its own.

### 5.5 Reliability

```
frame_loss_pct   = 100 * (1 - frames_received / frames_sent)
```
`frames_sent` is authoritative from the phone-side log, **not** inferred from
sequence gaps (a tail-end loss is invisible to gap analysis).

```
frame_gaps       = count of non-contiguous frame_seq jumps
reorder_events   = count of frames arriving with seq < max_seq_seen
corrupt_frames   = frames the decoder rejected
```

### 5.6 Jitter

Per RFC 3550, computed on inter-arrival deviation:

```
D(i) = (t_received[i] - t_received[i-1]) - (t_sent[i] - t_sent[i-1])
J(i) = J(i-1) + (|D(i)| - J(i-1)) / 16
```

Note `D(i)` is a *difference of differences*, so **clock offset cancels out** —
jitter is trustworthy even if §4 is imperfect.

### 5.7 Efficiency

```
goodput_mbps      = 8 * payload_bytes_total / duration_s / 1e6
wire_overhead_pct = 100 * (wire_bytes - payload_bytes) / payload_bytes
```

`wire_bytes` comes from a packet capture (§6.4), not from application counters —
application counters cannot see retransmissions, ACKs, or header overhead.

### 5.8 Statistics to report — never just the mean

For every latency metric report: **p50, p95, p99, max, stddev**.

> A protocol with a 40 ms mean and a 400 ms p99 is worse for real-time control
> than one with a 60 ms mean and a 70 ms p99. Means alone hide exactly the
> behaviour we care about.

---

## 6. Run protocol

### 6.1 Metadata block — logged for every run, no exceptions

```json
{
  "run_id": "20260811T140355Z-webrtc-5ghz-01",
  "protocol": "webrtc",
  "timestamp_utc": "2026-08-11T14:03:55Z",
  "operator": "farhan",

  "encoder": {
    "codec": "h264", "profile": "baseline", "level": "3.1",
    "width": 1280, "height": 720, "fps": 30,
    "bitrate_bps": 4000000, "bitrate_mode": "CBR",
    "keyframe_interval_s": 1, "b_frames": 0,
    "codec_name": "c2.qti.avc.encoder", "hardware_accelerated": true
  },

  "phone":  { "model": "", "android_version": "", "soc": "", "battery_pct": 0, "thermal_status": "" },
  "pc":     { "os": "", "cpu": "", "ram_gb": 0, "nic": "" },
  "network":{ "router_model": "", "band": "5GHz", "channel": 0, "channel_width_mhz": 80,
              "rssi_dbm": 0, "link_speed_mbps": 0, "other_clients": 0 },

  "run": { "duration_s": 120, "warmup_discarded_s": 10 },
  "clock_sync": { "offset_before_ns": 0, "offset_after_ns": 0, "drift_ns": 0, "status": "OK" },
  "validation": { "config_matches_frozen": true, "status": "VALID" }
}
```

### 6.2 Run rules

1. **Duration ≥ 120 s.** Shorter runs do not produce a meaningful p99.
2. **Discard the first 10 s.** Encoder ramp-up and transport handshake are not
   steady-state; including them biases every percentile.
3. **Three runs per configuration**, minimum. Report the median run. If the
   three disagree wildly, that instability *is* the finding — report it.
4. **Randomise protocol order** across sessions. Thermal throttling and WiFi
   congestion drift over a session; fixed order systematically penalises
   whatever is always measured last.
5. **Phone on mains power**, screen on, battery > 50 %. Android aggressively
   throttles the encoder on low battery or thermal pressure.
6. **Log `thermal_status`** before and after. A `THROTTLING` run gets flagged,
   not averaged in.

### 6.3 Network conditions

Measure each protocol under **all three** conditions:

| Condition | Setup | What it exposes |
|---|---|---|
| `clean` | 5 GHz, phone ≤ 2 m from router, no other traffic | Best case, lower bound |
| `realistic` | 5 GHz, phone ~8 m, one wall, background traffic | Expected deployment |
| `stressed` | 2.4 GHz, ~12 m, competing traffic | Where ARQ/FEC differences actually show |

> The `stressed` condition is where the protocols genuinely separate. A clean-LAN
> comparison mostly measures the encoder, and will make all seven look similar.

### 6.4 Packet capture

Run `tcpdump`/Wireshark on the PC side for one representative run per protocol.
Needed for §5.7 wire overhead, and the only way to see retransmissions.

---

## 7. Two-way requirement — upstream control channel

The issue is explicit:

> A protocol that streams beautifully but cannot carry or pair with a
> low-latency back-channel is not a winner for us.

So the upstream path is a **first-class part of the evaluation**, not an
afterthought.

### 7.1 Control command set

Modelled on the real fisheye control surface:

| Command | Payload | Notes |
|---|---|---|
| `SET_ALPHA` | float32 | Viewing angle α |
| `SET_BETA` | float32 | Viewing angle β |
| `SET_ZOOM` | float32 | Zoom factor |
| `SET_PRESET` | uint8 | Preset slot |
| `PING` | uint64 | Clock sync + RTT probe (§4) |

Wire format in `02-wire-format.md`.

### 7.2 What is measured upstream

- `control_rtt` — p50/p95/p99/max (§5.4)
- **Command loss rate** — does a `SET_ALPHA` ever silently vanish?
- **Ordering** — can command N+1 arrive before N? For a control surface, an
  out-of-order α update means the view jumps to a stale angle and *stays* there.
- **Behaviour under video load** — measure control RTT *while* video streams at
  full bitrate. Measuring it on an idle link is a meaningless best case.

### 7.3 Per-protocol back-channel classification

| Protocol | Native back-channel | Notes |
|---|---|---|
| WebRTC | ✅ `RTCDataChannel` | Same ICE path, SCTP; configurable reliability |
| SRT | ⚠️ bidirectional stream possible | Usable but awkward for discrete commands |
| RTSP/RTP | ⚠️ RTCP APP packets | Non-standard use; separate TCP control exists |
| Raw UDP | ❌ none | Must pair with a separate socket |
| MJPEG/HTTP | ❌ none | Must pair with a separate HTTP endpoint |
| RTMPS | ⚠️ AMF RPC | High latency; unsuitable for real-time control |
| HLS/DASH | ❌ none | Fundamentally one-way, pull-based |

For protocols with no native back-channel, measure the **paired approach**: a
separate UDP control socket alongside the video transport. That is a legitimate
architecture — but the report must state that it is two mechanisms to build,
secure, and keep alive, not one.

---

## 8. Deliverable

Final output is a table of measured values — no adjectives, no "feels smoother":

| Protocol | Transport p50/p95/p99 (ms) | Glass-to-glass p50 (ms) | Loss % | Jitter (ms) | Overhead % | Control RTT p50/p99 (ms) | Native back-channel | Encrypted |
|---|---|---|---|---|---|---|---|---|
| WebRTC | | | | | | | ✅ | ✅ DTLS-SRTP |
| SRT | | | | | | | ⚠️ | ✅ AES |
| RTSP/RTP | | | | | | | ⚠️ | ❌ |
| Raw UDP | | | | | | | ❌ | ❌ |
| MJPEG/HTTP | | | | | | | ❌ | ❌ |
| RTMPS | | | | | | | ⚠️ | ✅ TLS |
| HLS/DASH | | | | | | | ❌ | ⚠️ HTTPS |

Each cell traceable to a `run_id`. Raw logs committed alongside the report so
anyone can recompute.

---

## 9. Sign-off

Freeze before the first measurement:

- [ ] §2 encoder config agreed
- [ ] §3 instrumentation contract agreed
- [ ] §4 clock-sync method and its stated ±1–3 ms limitation accepted
- [ ] §5 metric formulas agreed
- [ ] §6.3 three network conditions agreed
- [ ] §7 control command set matches the real control surface

Changing §2 or §3 after measurement starts means **re-running everything
already measured**. That is the cost this document exists to avoid.
