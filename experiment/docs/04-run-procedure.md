# Run Procedure & Experiment Matrix

Operational companion to `01-harness-spec.md`. This is the checklist the
operator follows at the rig.

---

## 1. Build

### PC receiver

```powershell
.\experiment\build-receiver.ps1
```

Configures CMake, builds `ucv-receiver` / `ucv-selftest` / `ucv-mocksender` /
`ucv-nv12test` / `ucv-jpegtest` into `experiment\receiver\build\`, runs the
test suite, and
prints the IP to enter on the phone. Add `-Clean` for a from-scratch rebuild.

The script passes the compiler explicitly because this machine has MinGW-w64
(via winget) rather than MSVC, which CMake does not locate on its own. Binaries
are statically linked, so they run on a PC without the toolchain installed.

Release is the default — a receiver compiled `-O0` adds its own latency to the
numbers it reports.

> The build fails the run if `ctest` fails. That is deliberate: a bug in the
> measurement primitives does not crash, it produces plausible wrong numbers.

Manual equivalent, if you prefer:

```powershell
cmake -S experiment\receiver -B experiment\receiver\build -G "MinGW Makefiles" `
      -DCMAKE_CXX_COMPILER="<path>\g++.exe" -DCMAKE_MAKE_PROGRAM="<path>\mingw32-make.exe"
cmake --build experiment\receiver\build
cd experiment\receiver\build; ctest --output-on-failure
```

### Android sender

```powershell
.\experiment\install-phone.ps1
.\experiment\install-phone.ps1 -Clean    # after editing native C code
```

Checks for an authorised device (with the fix if there is not one), builds,
installs, and verifies the 12 JNI symbols actually made it into the `.so`.

> Use `-Clean` after touching native code. Gradle's CMake integration has been
> observed serving a stale `.so` even when the task reports as executed, which
> surfaces as `UnsatisfiedLinkError` for a symbol that is definitely there
> (see CLAUDE.md).

**Install while the phone is on USB, then unplug it** — the phone has one USB
port and the camera needs it.

---

## 2. Rig setup

### Topology

```
   Phone  ))) WiFi )))  Router  ---- LAN cable ----  PC
   camera + encode                                   ucv-receiver
```

PC wired, phone wireless, **same router**. This is the correct arrangement:
only the phone's WiFi link is under test, so the PC's own radio jitter never
enters the measurement.

> **Both endpoints must be on the same subnet.** If the PC also has WiFi
> enabled it will hold an address on a second subnet, and entering that one on
> the phone produces a run where no frames ever arrive and nothing reports an
> error. `Test-Connectivity.ps1` (below) catches this in seconds.

Confirm which address to use:

```powershell
.\experiment\Get-RigInfo.ps1
```

It prefers the wired interface and warns when the PC sits on more than one
subnet.

### Pre-flight

Once the phone is on the router's WiFi and you know its IP
(Settings → About → Status, or the router's client list):

```powershell
.\experiment\Test-Connectivity.ps1 -Phone 192.168.0.51
```

Checks subnet match, reachability, firewall rule for the video port, and that
a datagram can reach the phone's control port at all.

### Checklist — before every session

- [ ] Phone on **mains power**, battery > 50 %, screen on
- [ ] Phone `thermal_status` recorded (a `THROTTLING` phone silently changes
      encoder behaviour and invalidates comparisons)
- [ ] PC on **Ethernet**; note the negotiated link speed in the run metadata
- [ ] Router band/channel/width recorded; phone RSSI and link speed noted
- [ ] Other WiFi clients counted (or the band cleared)
- [ ] Camera plugged, confirmed streaming at the pinned mode
- [ ] `Test-Connectivity.ps1` passes
- [ ] Both clocks free-running (no NTP step mid-run — `CLOCK_MONOTONIC`
      protects against this, but note it anyway)

---

## 3. Running one measurement

Order matters. The phone's control channel must be up before the receiver
starts, or its clock-sync probe fails and it aborts the run — deliberately,
since every one-way latency figure depends on that offset.

**1. Phone** — Open USB Camera → grant permissions → enter the PC's IP →
**1. Start Control**. Note the `run_id` it displays.

**2. PC** — start the receiver with the *same* run_id:

```powershell
.\experiment\receiver\build\ucv-receiver.exe `
    --phone 192.168.0.51 `
    --protocol raw_udp `
    --run-id 20260812T101500Z-rawudp-5ghz-clean-01 `
    --duration 120 `
    --warmup 10 `
    --out results
```

**3. Phone** — **2. Start Raw UDP**.

The receiver prints a live line every 5 s and a full report at the end, and
writes `receiver-<run_id>.ndjson`. The phone's on-screen panel shows the
negotiated encoder and live `cap / dec / enc / sent` counters — the only way
to see the rig is healthy, since the USB port is taken by the camera.

> The run_id must match on both sides. If it does not, the receiver correctly
> discards every frame as belonging to a different run — which is the
> protection working, not a bug.

> **First run on a given phone is bring-up, not measurement.** See
> `03-implementation-plan.md` §11 for what to verify and what each failure
> mode means.

### Verifying the harness without a phone

`ucv-mocksender` speaks the exact wire protocol — useful for validating the
measurement path on one machine, or for training a new operator:

```bash
./ucv-mocksender --run-id TEST-01 --duration 30 --drop-permil 20 &
./ucv-receiver --phone 127.0.0.1 --run-id TEST-01 --duration 25 --warmup 3
```

> **Never put mock-sender numbers in the results table.** They describe the
> loopback path, not any protocol.

---

## 4. Experiment matrix

7 protocols × 3 network conditions × 3 repeats = **63 runs**, ~2 minutes each
plus setup. Budget roughly **6–8 hours of rig time**, split across sessions.

| Protocol | clean (5 GHz, 2 m) | realistic (5 GHz, 8 m, 1 wall) | stressed (2.4 GHz, 12 m, contended) |
|---|---|---|---|
| WebRTC | ×3 | ×3 | ×3 |
| SRT | ×3 | ×3 | ×3 |
| RTSP/RTP | ×3 | ×3 | ×3 |
| Raw UDP | ×3 | ×3 | ×3 |
| MJPEG/HTTP | ×3 | ×3 | ×3 |
| RTMPS | ×3 | ×3 | ×3 |
| HLS/DASH | ×3 | ×3 | ×3 |

### Two rules that matter more than they look

**Randomise protocol order within each session.** Thermal state and WiFi
congestion drift over hours; a fixed order systematically penalises whichever
protocol is always measured last. This is the easiest way to produce a
confident, wrong ranking.

**Interleave conditions, do not batch them.** Running all `clean` on Monday and
all `stressed` on Friday confounds the condition with everything else that
changed between Monday and Friday.

### Additional SRT sub-matrix

SRT's latency is dominated by `SRTO_LATENCY`, so it gets its own sweep at
20 / 50 / 120 ms. Reporting a single SRT number without stating the setting
would be meaningless.

---

## 5. Per-run record

Fill this in immediately — reconstructing it later is guesswork:

```
run_id          : 20260812T101500Z-rawudp-5ghz-clean-01
protocol        : raw_udp
condition       : clean
operator        :
phone model     :          android:          soc:
battery %       :          thermal before:        after:
router          :          band:       channel:      width:
rssi dbm        :          link speed:            other clients:
camera mode     : 1280x720@30 MJPEG
encoder         : h264 baseline 3.1, 4 Mbps CBR, 1 s GOP, codec name:
hardware enc    : yes / no        ← "no" INVALIDATES the run
clock drift ms  :                 ← > 2 ms ⇒ CLOCK_SUSPECT
notes           :
```

---

## 6. Analysis

```bash
python experiment/analysis/analyze.py results/*.ndjson --out summary.md
```

Produces the comparison table of `01-harness-spec.md` §8, plus per-protocol
percentile breakdowns. Because the logs hold **raw timestamps only**, a
corrected metric formula can be re-applied to historical runs without
re-measuring — which matters, since re-measuring means re-booking the rig.

---

## 7. Reading the results honestly

Four traps worth naming in advance, because each one produces a confident
conclusion that is wrong:

**Do not rank on differences below ~5 ms.** Clock sync carries ±1–3 ms of
residual error (harness spec §4.2). If two protocols sit within 5 ms, the
honest statement is "not resolvable by this harness" — not a winner.

**Do not rank on the mean.** A 40 ms mean with a 400 ms p99 is worse for
real-time control than 60 ms mean with 70 ms p99. Rank on p95/p99.

**Do not compare MJPEG's bandwidth to the H.264 protocols.** It carries JPEG
and has no inter-frame compression; the comparison is meaningless by
construction. Its latency and control numbers *are* comparable.

**Do not let `clean` decide it.** On a quiet LAN every protocol looks similar
because the encoder dominates. The `stressed` condition is where ARQ, FEC and
congestion control actually separate — that is the column that should carry the
most weight for a deployment decision.

And the one the issue itself insists on: **a protocol that wins downstream but
has no usable back-channel has not won.** Weight the control-RTT column
accordingly, and note in the report which protocols needed a second, separately
maintained mechanism to get one.
