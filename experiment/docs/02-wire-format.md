# Wire Format Specification

Binary contract shared by the Android sender and the C++ receiver. Both sides
implement this from the same document; a mismatch here silently corrupts every
measurement, so the receiver validates aggressively and refuses malformed input
rather than guessing.

All multi-byte integers are **little-endian**. Rationale: both ends are LE
(arm64, x86-64), so this avoids a byte swap on every frame in the hot path.
Floats are IEEE-754 binary32/64, also LE.

---

## 1. Frame preamble (downstream: phone → PC)

Fixed **56 bytes** for Raw UDP frame fragments. Used directly by
Raw UDP, SRT, and MJPEG-over-HTTP's binary variant; carried inside RTP header
extensions or AMF metadata for the protocols that cannot take opaque bytes
(see `01-harness-spec.md` §3.2).

```
 offset  size  field            type      description
 ──────  ────  ───────────────  ────────  ─────────────────────────────────────
      0     4  magic            uint32    0x55435631 ("UCV1")
      4     1  version          uint8     frame format version = 2
      5     1  flags            uint8     bit0 = keyframe
                                          bit1 = fragmented
                                          bit2 = last fragment
                                          bit3..7 reserved (must be 0)
      6     2  protocol_id      uint16    see §4
      8     4  frame_seq        uint32    monotonic, never reset within a run
     12     4  packet_seq       uint32    monotonic for every UDP datagram
     16     4  payload_bytes    uint32    encoded frame size, excl. preamble
     20     2  fragment_index   uint16    zero-based index within frame
     22     2  fragment_count   uint16    total datagrams for this frame
     24     8  t_capture_ns     uint64    phone CLOCK_MONOTONIC
     32     8  t_encoded_ns     uint64    phone CLOCK_MONOTONIC
     40     8  t_sent_ns        uint64    phone CLOCK_MONOTONIC
     48     4  run_id_hash      uint32    FNV-1a of the run_id string
     52     4  header_crc32     uint32    CRC-32 of bytes [0..51]
 ──────  ────
    56 bytes total
```

### Field notes

**`magic`** — lets the receiver resynchronise after loss on a stream transport,
and rejects stray traffic on the UDP port. Cheap, and it has caught real bugs.

**`frame_seq`** — the basis of loss and reorder detection. Must **not** reset on
reconnect; a reset is indistinguishable from massive reordering.

**`t_sent_ns`** — stamped as late as possible, immediately before the transport
write call. Stamping it earlier folds application queueing into the transport
number and flatters slow transports.

**`run_id_hash`** — guards against cross-contamination when a stale sender is
still running. Frames whose hash does not match the receiver's expected run are
**discarded and counted**, never silently accepted.

**`header_crc32`** — header only, not payload. A corrupt header yields a garbage
`frame_seq`, which would poison loss statistics far worse than a dropped frame.
Payload integrity is left to the transport (or to the decoder rejecting it).

### 1.1 Fragmentation (UDP-family only)

UDP datagrams must stay under the path MTU to avoid IP-layer fragmentation,
which turns one lost fragment into a lost frame with no visibility. A 720p
H.264 keyframe at 4 Mbps routinely exceeds that.

- **Fragment payload size: 1200 bytes.** Conservative and leaves room for the
  56-byte header plus VPN/tunnel overhead.
- Every fragment carries a **full 56-byte preamble** with identical
  `frame_seq`, `t_*`, and `payload_bytes` (the size of the *whole* frame).
- `flags` bit1 set on all fragments; bit2 set only on the last.
- `packet_seq` exposes datagram gaps; `fragment_index/count` identifies every
  fragment explicitly for loss and reorder diagnostics.

### 1.2 MJPEG/HTTP header variant

The existing MJPEG server is text/multipart, so the same fields travel as HTTP
headers inside each part — keeping the stream viewable in a plain browser:

```http
--frame
Content-Type: image/jpeg
Content-Length: 45231
X-UCV-Seq: 1234
X-UCV-Cap-Ns: 123456789012345
X-UCV-Enc-Ns: 123456789112345
X-UCV-Snd-Ns: 123456789122345
X-UCV-Key: 1
X-UCV-Run: 3735928559
X-UCV-Net-Stats: 1
X-UCV-TCP-Segments: 4821
X-UCV-TCP-Retrans: 17
```

`X-UCV-TCP-Segments` and `X-UCV-TCP-Retrans` are connection-relative counters
sampled from Android `TCP_INFO`. The receiver subtracts a second baseline after
warmup and reports `retrans / (data_segments + retrans)` as the realtime TCP
retransmission rate over all send attempts. This exposes network loss recovered
by TCP; it is not final media loss.
`X-UCV-Net-Stats: 0` means the kernel did not expose `TCP_INFO`, in which case
the dashboard falls back to observed frame gaps instead of inventing a packet
loss value. Header text overhead is counted in `wire_overhead_pct`, not excused.

---

## 2. Control message (upstream: PC → phone)

Fixed **32 bytes**. Small enough to always fit one datagram — no fragmentation,
so a control command can never be half-delivered.

```
 offset  size  field            type      description
 ──────  ────  ───────────────  ────────  ─────────────────────────────────────
      0     4  magic            uint32    0x55435643 ("UCVC")
      4     1  version          uint8     = 1
      5     1  cmd_type         uint8     see §2.1
      6     2  flags            uint16    bit0 = ack_requested
      8     4  cmd_seq          uint32    monotonic per session
     12     8  t_sent_ns        uint64    PC CLOCK_MONOTONIC
     20     8  payload          8 bytes   interpretation depends on cmd_type
     28     4  crc32            uint32    CRC-32 of bytes [0..27]
 ──────  ────
    32 bytes total
```

### 2.1 Command types

| Value | Name | Payload layout |
|---|---|---|
| `0x01` | `SET_ALPHA` | float32 at +20, 4 bytes padding |
| `0x02` | `SET_BETA` | float32 at +20, 4 bytes padding |
| `0x03` | `SET_ZOOM` | float32 at +20, 4 bytes padding |
| `0x04` | `SET_PRESET` | uint8 at +20, 7 bytes padding |
| `0x05` | `PING` | uint64 opaque token at +20, echoed in the ACK |
| `0x06` | `START_RUN` | uint32 run_id_hash at +20, uint32 duration_s at +24 |
| `0x07` | `STOP_RUN` | unused |

### 2.2 ACK message

Phone → PC, **40 bytes**. Carries the phone-side receive and send timestamps so
the PC can compute the clock offset per §4 of the harness spec.

```
 offset  size  field            type      description
 ──────  ────  ───────────────  ────────  ─────────────────────────────────────
      0     4  magic            uint32    0x55435641 ("UCVA")
      4     1  version          uint8     = 1
      5     1  cmd_type         uint8     echo of the command being ACKed
      6     2  flags            uint16    bit0 = command applied successfully
      8     4  cmd_seq          uint32    echo
     12     8  t_cmd_sent_ns    uint64    echo of the command's t_sent_ns (t1)
     20     8  t_recv_ns        uint64    phone clock on arrival (t2)
     28     8  t_ack_ns         uint64    phone clock just before ACK send (t3)
     36     4  crc32            uint32    CRC-32 of bytes [0..35]
 ──────  ────
    40 bytes total
```

The PC stamps `t4` on arrival. With `(t1,t2,t3,t4)` it computes RTT and clock
offset exactly as in harness spec §4.1 — which is why `PING` and the ACK share
one mechanism rather than having a separate sync protocol.

---

## 3. Session log format (both sides)

Newline-delimited JSON (`.ndjson`) — appendable, streamable, and a truncated
file from a crashed run still parses up to the last complete line.

### 3.1 Sender log — `sender-<run_id>.ndjson`

First line is always the metadata block from harness spec §6.1:

```json
{"type":"meta","run_id":"...","protocol":"raw_udp","encoder":{...},"phone":{...}}
```

Then one line per frame:

```json
{"type":"frame","seq":1234,"cap_ns":123456789012345,"enc_ns":...,"snd_ns":...,"bytes":45231,"key":false}
```

Terminated by a summary line — the authoritative `frames_sent` for §5.5, since
sequence-gap analysis cannot see a tail-end loss:

```json
{"type":"summary","frames_sent":3600,"bytes_sent":163840000,"encoder_drops":0,"duration_ns":120000000000}
```

### 3.2 Receiver log — `receiver-<run_id>.ndjson`

```json
{"type":"meta","run_id":"...","protocol":"raw_udp","pc":{...},"clock_offset_ns":-1234567}
{"type":"frame","seq":1234,"cap_ns":...,"enc_ns":...,"snd_ns":...,"rcv_ns":...,"dec_ns":...,"bytes":45231,"key":false}
{"type":"loss","seq_from":1240,"seq_to":1242,"count":2}
{"type":"control","cmd_seq":57,"cmd_type":1,"t1_ns":...,"t2_ns":...,"t3_ns":...,"t4_ns":...}
{"type":"summary","frames_received":3597,"frames_lost":3,"reorder_events":0,"corrupt":0,"reassembly_failures":0}
```

> Receiver logs record **raw timestamps only** — never derived latencies. All
> derivation happens in analysis, so a fix to a metric formula can be re-applied
> to old runs without re-measuring.

---

## 4. Protocol IDs

| ID | Protocol |
|---|---|
| `1` | WebRTC |
| `2` | SRT |
| `3` | RTSP/RTP over UDP |
| `4` | Raw UDP (MPEG-TS or bare) |
| `5` | MJPEG over HTTP |
| `6` | RTMPS |
| `7` | HLS/DASH |

---

## 5. Default ports

| Port | Purpose |
|---|---|
| `8181` | MJPEG over HTTP (existing) |
| `8200` | Control channel (UDP, bidirectional) — all protocols |
| `8201` | Raw UDP video |
| `8202` | SRT |
| `8554` | RTSP |
| `1935` | RTMPS |
| `8203` | WebRTC signalling (WebSocket) |
| `8204` | HLS/DASH HTTP |

Control is a **dedicated port for every protocol**, including those with a
native back-channel. That keeps the control measurement methodology identical
across protocols; where a native channel exists (WebRTC data channel), it is
measured *additionally* so the paired-vs-native comparison is apples-to-apples.
