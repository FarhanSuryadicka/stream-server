# Future Architecture — Mobile Edge Processing and C++ Control-Room Server

Status: **future direction only — do not implement yet**  
Date recorded: 11 August 2026

## 1. Instruction to future development sessions

This document records a possible production architecture after the transport
research is complete. It is not authorization to begin rewriting the receiver,
dashboard, or server in C++.

The current priority remains:

1. finish the planned transport protocols;
2. validate every implementation end-to-end using the phone and USB camera;
3. measure latency, jitter, packet loss/retransmission, reliability, and
   goodput;
4. repeat measurements across supported camera resolutions;
5. compare the protocols using the same test methodology;
6. select the production transport from evidence.

**Do not start the C++ production server, Qt UI, LVGL UI, database migration,
multi-room service, or recording platform unless the user explicitly requests
that work in a future session.**

The current Python/HTML dashboard and PowerShell scripts remain the active
research harness. Do not remove or replace them while protocol comparison is
still in progress.

## 2. Current research scope

The repository currently exists to answer the transport question fairly, not
to build the final elderly-care control-room product.

Current protocol status at the time of this decision:

| Protocol | Status |
|---|---|
| Raw UDP H.264 | Implemented and tested phone-to-PC |
| MJPEG over HTTP | Implemented and tested, including preview |
| RTP/UDP H.264 | Data plane implemented; RTSP signalling not implemented |
| SRT H.264 | Android and PC implementation build successfully; physical phone test pending |
| WebRTC | Not implemented |
| RTMPS | Not implemented |
| HLS/DASH | Not implemented |

RTP/UDP must not be called complete RTSP until `DESCRIBE`, `SETUP`, `PLAY`, and
SDP signalling exist and are validated.

SRT must not be described as physically validated until the current APK is
installed and tested with the phone and camera. Its initial research profile is
live/message mode, 20 ms configured latency, and encryption disabled.

## 3. Intended long-term system boundary

The potential production system is divided into two logical sides:

```text
1. Mobile / in-room edge device
2. Server / surveillance control room
```

The design goal is to put image-processing and AI workload on each mobile edge
device so the control room can operate on a modest mini PC instead of a
powerful centralized AI workstation.

```text
Fisheye camera
      |
      v
Android smartphone
  - UVC capture
  - MOIL/remap/dewarp
  - anypoint/virtual PTZ
  - AI inference and tracking
  - hardware video encoding
  - AI event generation
      |
      | encoded video + reliable events/telemetry
      v
Control-room mini PC
  - receive streams from multiple rooms
  - display selected streams
  - receive and surface alerts
  - record compressed video
  - search and replay events
  - manage devices, users, retention, and health
```

This follows the direction described in:

- `draft-smartphone-fisheye project.pdf`;
- `Elderly_Care_AI_Monitoring_Concept_Review.pdf`;
- `Server Side Development Plan - Smart Room Surveillance.docx`.

## 4. Mobile-side responsibilities

The mobile side is intended to perform the heavy per-camera work:

- USB UVC capture;
- fisheye remap/dewarp;
- anypoint perspective generation;
- alpha, beta, zoom, and preset handling;
- image enhancement;
- human, fall, pose, inactivity, activity, and anomaly inference;
- object/person tracking;
- hardware H.264 encoding;
- main-stream and substream generation;
- structured AI-event generation;
- short local buffering when the network is unavailable;
- device telemetry and heartbeat;
- remote command handling.

The existing implementation style remains suitable:

```text
Kotlin + Jetpack Compose  : Android UI, permissions, lifecycle
C/C++ NDK                 : UVC, remap, media pipeline, transport
MediaCodec                : hardware H.264 encoding
Mobile AI runtime         : to be selected after feasibility tests
```

AI placement is still subject to device feasibility testing. The preferred
production direction is mobile-side AI, but accuracy, inference latency,
thermal behavior, power consumption, and long-duration stability must be
measured on the actual phone.

## 5. Future server-side responsibilities

The control-room server should avoid repeating work already completed by the
mobile devices. Its future responsibilities are expected to include:

- authenticate and manage multiple room devices;
- ingest compressed video streams;
- receive structured AI events over a reliable channel;
- decode only streams needed for display or verification;
- show room status and real-time alerts;
- record compressed video without re-encoding;
- provide search, replay, verification, and export;
- manage circular retention and storage health;
- maintain rooms, devices, events, users, and configuration;
- enforce role-based access control;
- maintain an audit trail;
- send virtual PTZ/remap commands to mobile devices;
- show device, camera, network, thermal, application, and model health;
- optionally run server-side AI only as a fallback if mobile AI is not viable.

The server should not normally perform fisheye dewarping, image enhancement,
primary tracking, or primary AI inference.

## 6. Recommended future server technology

When the user authorizes production-server development, the recommended stack
is:

```text
Core/backend         : C++20
Desktop UI           : Qt 6 with QML
Media                : FFmpeg
Transport            : selected protocol from this research
Local metadata       : SQLite initially
Recording            : segmented MP4 or MKV files
Optional AI fallback : ONNX Runtime C++
Build                : CMake
```

Qt/QML is preferred over LVGL for the main control-room application because
the expected UI includes multi-camera video, alerts, tables, analytics,
search, replay timelines, reports, settings, user management, and potentially
multiple monitors.

LVGL remains technically possible and may be appropriate for a separate
embedded touchscreen or simple wall-status panel. It is not the current
recommendation for the primary Windows/Linux surveillance workstation.

This technology choice is also a future decision. Do not add Qt, LVGL, FFmpeg
server integration, or ONNX Runtime merely because they appear in this
document.

## 7. Suggested server process model

For a production elderly-care system, fault isolation is preferable to putting
recording and UI lifecycle in the same thread or tightly coupled process.

Potential future packaging:

```text
monitor-core
  - device sessions
  - transport ingestion
  - recording
  - events and alerts
  - database
  - telemetry and health

monitor-ui
  - Qt/QML control-room interface
  - live grid and focus view
  - alert handling
  - search, replay, reports, and settings
```

The two processes could communicate through local IPC. This would allow
recording and device connections to remain active if the UI is restarted.

An initial implementation may still begin as a modular monolith, provided the
core and UI boundaries are kept explicit. This is a design note, not an active
implementation request.

## 8. Keeping the mini PC lightweight

Moving AI and dewarp to mobile is necessary but not sufficient. The server
must also avoid decoding and re-encoding every full-resolution stream.

### Main stream and substream

Each mobile device should eventually be able to provide:

```text
Main stream
  - higher resolution and frame rate
  - fullscreen view
  - recording
  - event verification

Substream
  - low resolution and lower frame rate
  - multi-room overview grid
  - low-cost background monitoring
```

Illustrative profiles, to be validated rather than hard-coded:

```text
Main      : 1920x1080 at 15-30 fps
Substream : 480x270 or 640x360 at 5-10 fps
```

The grid should display substreams. Only a selected room or active alert should
switch to the main stream.

### Decode on demand

The server should decode only streams visible on the current page, selected by
the operator, or required for event verification. Devices on other pages can
remain represented by status, telemetry, and a recent snapshot.

### Record without re-encoding

Preferred recording path:

```text
compressed H.264 received from mobile
              |
              v
remux directly into segmented MP4/MKV
```

Avoid this path unless transformation is genuinely required:

```text
H.264 -> decode -> raw frame -> encode H.264 again
```

Direct remux reduces CPU/GPU load, preserves quality, and avoids additional
encoding latency.

### Hardware decoding

The future mini PC should provide hardware H.264 decoding. Decoded video should
move to GPU-backed rendering with as few CPU copies as practical.

A reasonable initial hardware class would include:

- integrated hardware H.264 decoder;
- 16 GB RAM as an initial baseline;
- Gigabit Ethernet;
- NVMe for the application, database, and cache;
- separate high-capacity storage or NAS for long retention;
- adequate cooling for continuous operation;
- UPS and automatic restart/watchdog support.

No exact room count should be promised from these specifications alone.
Capacity must be established by load testing the chosen resolution, FPS,
bitrate, number of simultaneous decoders, recording policy, and UI layout.

## 9. Future communication-plane separation

Video, safety events, control, and telemetry should not depend on one shared
delivery mechanism.

### Video plane

The production video protocol must be selected from the current research.
SRT is a strong candidate for lossy Wi-Fi because it offers recovery and
configurable latency, while RTP/RTSP may be preferable when CCTV
interoperability is required. No final choice has been made.

### Event plane

Safety-related events should use a reliable, authenticated, encrypted channel
with:

- unique event ID;
- device and room identity;
- event type and severity;
- capture timestamp;
- confidence and model version;
- ACK and retry;
- deduplication;
- local mobile queue during disconnection;
- optional snapshot or clip reference.

An alarm must not be carried only as metadata inside the video stream. The
video path may be delayed or interrupted while the event still needs reliable
delivery.

### Control plane

Commands should use versioned request/response messages with command IDs,
timeouts, ACK/NACK, and explicit error codes. Expected commands include:

```text
SET_VIEW
SET_ALPHA
SET_BETA
SET_ZOOM
SET_PRESET
START_STREAM
STOP_STREAM
SET_BITRATE
GET_STATUS
PING
```

### Telemetry plane

Expected device telemetry includes:

- camera connected/open state;
- capture, remap, AI, encode, and send FPS;
- bitrate and dropped frames;
- network RTT/loss/retransmission;
- CPU/thermal status;
- battery and charging status;
- application and AI-model versions;
- uptime and last error.

Logical separation does not necessarily require four physical sockets, but the
wire contract must distinguish their reliability, priority, and retry rules.

## 10. Storage direction

For an initial single-site mini PC:

```text
SQLite              : rooms, devices, users, events, configuration, indexes
Segmented MP4/MKV   : video recordings
Filesystem          : event snapshots and exported clips
```

Do not store large video recordings as SQLite blobs.

Recording should use short segments and a FIFO/circular retention policy so
the system can remove old footage without rewriting large files. Retention,
storage encryption, access logging, and legal/privacy requirements must be
defined before a real elderly-care pilot.

Storage sizing may dominate mini-PC design even when CPU usage is low. For
example, a constant 2 Mbps stream is approximately 21.6 GB per camera per day,
before filesystem/container overhead. Actual sizing must use measured mobile
bitrates and the selected recording policy.

## 11. Alert behavior and human verification

Mobile AI output should be treated as a detected condition requiring operator
verification, not as an infallible medical or safety conclusion.

Example UI wording:

```text
Possible fall detected
Room: 101
Confidence: 91%
[Open live view] [Replay preceding clip] [Acknowledge]
```

The server should retain the AI model version, confidence, relevant snapshot or
clip, operator acknowledgement, and audit record.

## 12. Research completion gate

Production-server work should begin only after the user explicitly authorizes
it. A sensible research-completion gate is:

- planned protocols implemented or explicitly removed from scope;
- every candidate built on Android and PC;
- physical camera tests completed;
- identical test procedure used across candidates;
- multiple repeated runs per resolution and protocol;
- loss/retransmission semantics documented per protocol;
- latency, jitter, reliability, goodput, CPU, thermal, and power implications
  reviewed;
- one or more production transport candidates selected;
- mobile/server streaming and event contracts agreed;
- expected room count and retention requirements available.

Until this gate is reached, continue using the existing experiment harness and
focus development on transport correctness and fair comparison.

## 13. Immediate next work — current priority

The next development session should continue from the existing transport
working tree, not from this future architecture.

Recommended immediate order:

1. review and preserve the current SRT changes;
2. install and physically validate SRT only when the user permits HP access;
3. verify SRT live preview and real-time detected-loss/recovered counters;
4. complete the next planned transport;
5. decide whether to finish RTSP signalling over the existing RTP data plane;
6. run the agreed resolution/protocol comparison matrix;
7. document results and limitations.

Any future agent reading this document must treat the C++ production server as
a recorded architectural direction only. The user will decide when that work
starts.

