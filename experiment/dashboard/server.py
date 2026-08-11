#!/usr/bin/env python3
"""Local web dashboard for UCV experiment runs.

Uses only the Python standard library. It can launch the native receiver,
tail its console output, and recompute all derived metrics from raw NDJSON.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import math
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time
import webbrowser
import zlib
from collections import deque
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


DASHBOARD_DIR = Path(__file__).resolve().parent
EXPERIMENT_DIR = DASHBOARD_DIR.parent
REPO_DIR = EXPERIMENT_DIR.parent
DEFAULT_RESULTS = EXPERIMENT_DIR / "results"
RECEIVER_EXE = EXPERIMENT_DIR / "receiver" / "build" / "ucv-receiver.exe"
LABELS_FILE = ".dashboard-labels.json"
SAFE_TEXT = re.compile(r"^[A-Za-z0-9_.@-]{1,96}$")
# Operator-authored comparison label. Deliberately looser than SAFE_TEXT —
# spaces and a few separators make a group name readable ("720p wifi 5GHz") —
# but no angle brackets, quotes or control characters, since these strings are
# rendered in the dashboard and written to the labels JSON.
GROUP_TEXT = re.compile(r"^[A-Za-z0-9 _.@:+/()x-]{1,64}$")
MODE_TEXT = re.compile(r"^\d{2,5}x\d{2,5}@\d{1,3}$")
PREVIEW_MAGIC = 0x31565055
PREVIEW_JPEG_MAGIC = 0x314A5055
PREVIEW_HEADER = struct.Struct("<IIIHH")
CONTROL_PACKET = struct.Struct("<IBBHIQ8sI")
ACK_PACKET = struct.Struct("<IBBHIQQQI")
UCV_MAGIC_CONTROL = 0x55435643
UCV_MAGIC_ACK = 0x55435641
UCV_CMD_GET_MODE = 0x09
UCV_CMD_STOP_RUN = 0x07
UCV_CTL_ACK_REQUESTED = 0x0001


def unique_run_id(results_dir: Path, requested: str) -> str:
    """Return a run ID whose NDJSON path does not already exist."""
    run_id = requested
    suffix = 2
    while (results_dir / f"receiver-{run_id}.ndjson").exists():
        run_id = f"{requested}-{suffix}"
        suffix += 1
    return run_id
UCV_ACK_APPLIED = 0x0001


def query_camera_modes(phone: str, port: int = 8200) -> list[str]:
    """Read the MJPEG modes advertised by the opened UVC camera."""
    peer = (str(ipaddress.ip_address(phone)), port)
    modes: list[str] = []
    seen: set[str] = set()
    sequence = int(time.time() * 1000) & 0xFFFFFFFF
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.6)
        for index in range(256):
            sequence = (sequence + 1) & 0xFFFFFFFF
            payload = struct.pack("<H", index) + bytes(6)
            head = CONTROL_PACKET.pack(
                UCV_MAGIC_CONTROL, 1, UCV_CMD_GET_MODE,
                UCV_CTL_ACK_REQUESTED, sequence, time.monotonic_ns(), payload, 0,
            )
            packet = head[:-4] + struct.pack("<I", zlib.crc32(head[:-4]) & 0xFFFFFFFF)
            reply = None
            for _ in range(2):
                sock.sendto(packet, peer)
                try:
                    candidate, _ = sock.recvfrom(256)
                except socket.timeout:
                    continue
                if len(candidate) != ACK_PACKET.size:
                    continue
                fields = ACK_PACKET.unpack(candidate)
                if (fields[0] == UCV_MAGIC_ACK and fields[1] == 1 and
                        fields[2] == UCV_CMD_GET_MODE and fields[4] == sequence and
                        zlib.crc32(candidate[:-4]) & 0xFFFFFFFF == fields[-1]):
                    reply = fields
                    break
            if reply is None:
                if index == 0:
                    raise ValueError("camera agent tidak menjawab; buka kamera di aplikasi HP")
                break
            if not (reply[3] & UCV_ACK_APPLIED):
                break
            width = (reply[6] >> 32) & 0xFFFF
            height = reply[6] & 0xFFFF
            fps = (reply[7] >> 32) & 0xFF
            mode = f"{width}x{height}@{fps}"
            if width and height and fps and mode not in seen:
                seen.add(mode)
                modes.append(mode)
    if not modes:
        raise ValueError("kamera terbuka tetapi tidak menawarkan mode MJPEG")
    return modes


def group_camera_modes(advertised: list[str]) -> list[dict]:
    """Group camera-advertised MJPEG modes without inventing resolutions."""
    parsed: dict[tuple[int, int], set[int]] = {}
    for mode in advertised:
        match = re.fullmatch(r"(\d+)x(\d+)@(\d+)", mode)
        if not match:
            continue
        width, height, fps = map(int, match.groups())
        parsed.setdefault((width, height), set()).add(fps)
    return [
        {"width": width, "height": height, "fps": sorted(fps_values, reverse=True)}
        for (width, height), fps_values in sorted(
            parsed.items(), key=lambda item: item[0][0] * item[0][1]
        )
    ]


def stop_phone_pipeline(phone: str, port: int = 8200) -> None:
    """Best-effort STOP used when the dashboard terminates the PC process."""
    try:
        peer = (str(ipaddress.ip_address(phone)), port)
        sequence = int(time.time() * 1000) & 0xFFFFFFFF
        head = CONTROL_PACKET.pack(
            UCV_MAGIC_CONTROL, 1, UCV_CMD_STOP_RUN,
            UCV_CTL_ACK_REQUESTED, sequence, time.monotonic_ns(), bytes(8), 0,
        )
        packet = head[:-4] + struct.pack("<I", zlib.crc32(head[:-4]) & 0xFFFFFFFF)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(5.0)
            sock.sendto(packet, peer)
            sock.recvfrom(256)
    except (OSError, ValueError):
        pass


class PreviewPipeline:
    """Best-effort H.264/JPEG preview, isolated from measurements."""

    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.latest_jpeg: bytes | None = None
        self.frame_number = 0
        self.status = "idle"
        self.error = ""
        self.sock: socket.socket | None = None
        self.ffmpeg: subprocess.Popen[bytes] | None = None
        self.stop_event = threading.Event()

    def _find_ffmpeg(self) -> str | None:
        found = shutil.which("ffmpeg")
        if found:
            return found
        known = Path(r"C:\Tools\ffmpeg\bin\ffmpeg.exe")
        return str(known) if known.is_file() else None

    def start(self, protocol: str) -> int:
        self.stop()
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(0.5)
        port = int(udp.getsockname()[1])
        process = None
        if protocol in ("raw_udp", "rtp_udp", "srt"):
            ffmpeg = self._find_ffmpeg()
            if not ffmpeg:
                udp.close()
                raise ValueError("FFmpeg tidak ditemukan; install FFmpeg atau tambahkan ke PATH")
            flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            command = [
                ffmpeg, "-hide_banner", "-loglevel", "error",
                "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
                "-f", "h264", "-i", "pipe:0", "-an",
                "-vf", "scale='min(960,iw)':-2",
                "-f", "image2pipe", "-c:v", "mjpeg", "-q:v", "5", "pipe:1",
            ]
            try:
                process = subprocess.Popen(
                    command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL, creationflags=flags,
                )
            except OSError:
                udp.close()
                raise ValueError("FFmpeg gagal dijalankan")
        self.sock = udp
        self.ffmpeg = process
        self.stop_event.clear()
        with self.condition:
            self.latest_jpeg = None
            self.frame_number += 1
            self.status = "waiting"
            self.error = ""
            self.condition.notify_all()
        threading.Thread(target=self._receive_frames, daemon=True).start()
        if process:
            threading.Thread(target=self._read_jpegs, daemon=True).start()
        return port

    def stop(self) -> None:
        self.stop_event.set()
        sock, process = self.sock, self.ffmpeg
        self.sock = None
        self.ffmpeg = None
        if sock:
            sock.close()
        if process:
            try:
                if process.stdin:
                    process.stdin.close()
            except OSError:
                pass
            if process.poll() is None:
                process.terminate()
        with self.condition:
            if self.status not in ("idle", "error"):
                self.status = "stopped"
            self.condition.notify_all()

    def _set_error(self, message: str) -> None:
        with self.condition:
            self.status = "error"
            self.error = message
            self.condition.notify_all()

    def _receive_frames(self) -> None:
        sock, process = self.sock, self.ffmpeg
        if not sock:
            return
        pending: dict[int, dict] = {}
        while not self.stop_event.is_set():
            try:
                packet, _ = sock.recvfrom(65535)
            except socket.timeout:
                now = time.monotonic()
                pending = {k: v for k, v in pending.items() if now - v["at"] < 2.0}
                continue
            except OSError:
                break
            if len(packet) < PREVIEW_HEADER.size:
                continue
            magic, seq, frame_bytes, index, count = PREVIEW_HEADER.unpack_from(packet)
            if magic not in (PREVIEW_MAGIC, PREVIEW_JPEG_MAGIC) or not count or index >= count or frame_bytes > 32 * 1024 * 1024:
                continue
            item = pending.setdefault(seq, {
                "size": frame_bytes, "count": count, "parts": {}, "at": time.monotonic(),
            })
            if item["size"] != frame_bytes or item["count"] != count:
                pending.pop(seq, None)
                continue
            item["parts"][index] = packet[PREVIEW_HEADER.size:]
            if len(item["parts"]) != count:
                continue
            frame = b"".join(item["parts"].get(i, b"") for i in range(count))
            pending.pop(seq, None)
            if len(frame) != frame_bytes:
                continue
            if magic == PREVIEW_JPEG_MAGIC:
                if not (frame.startswith(b"\xff\xd8") and frame.endswith(b"\xff\xd9")):
                    continue
                with self.condition:
                    self.latest_jpeg = frame
                    self.frame_number += 1
                    self.status = "live"
                    self.condition.notify_all()
            elif process and process.stdin:
                try:
                    process.stdin.write(frame)
                    process.stdin.flush()
                except (BrokenPipeError, OSError):
                    if not self.stop_event.is_set():
                        self._set_error("FFmpeg berhenti saat membaca H.264")
                    break

    def _read_jpegs(self) -> None:
        process = self.ffmpeg
        if not process or not process.stdout:
            return
        buffer = bytearray()
        while not self.stop_event.is_set():
            chunk = process.stdout.read(16384)
            if not chunk:
                break
            buffer.extend(chunk)
            while True:
                start = buffer.find(b"\xff\xd8")
                if start < 0:
                    if len(buffer) > 2:
                        del buffer[:-2]
                    break
                end = buffer.find(b"\xff\xd9", start + 2)
                if end < 0:
                    if start:
                        del buffer[:start]
                    break
                jpeg = bytes(buffer[start:end + 2])
                del buffer[:end + 2]
                with self.condition:
                    self.latest_jpeg = jpeg
                    self.frame_number += 1
                    self.status = "live"
                    self.condition.notify_all()
        if not self.stop_event.is_set():
            with self.condition:
                if self.latest_jpeg is None:
                    self.status = "error"
                    self.error = "FFmpeg berhenti sebelum menghasilkan gambar"
                    self.condition.notify_all()

    def snapshot(self) -> dict:
        with self.condition:
            return {"status": self.status, "error": self.error,
                    "frames": self.frame_number, "available": self.latest_jpeg is not None}

    def wait_frame(self, after: int, timeout: float = 5.0) -> tuple[int, bytes | None]:
        with self.condition:
            self.condition.wait_for(
                lambda: self.frame_number != after or self.stop_event.is_set(), timeout=timeout,
            )
            return self.frame_number, self.latest_jpeg


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = p * (len(ordered) - 1)
    lo, hi = math.floor(idx), math.ceil(idx)
    if lo == hi:
        return ordered[lo]
    frac = idx - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def downsample(points: list[dict], limit: int = 320) -> list[dict]:
    if len(points) <= limit:
        return points
    step = (len(points) - 1) / (limit - 1)
    return [points[round(i * step)] for i in range(limit)]


class DashboardState:
    def __init__(self, results_dir: Path):
        self.results_dir = results_dir.resolve()
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.process: subprocess.Popen[str] | None = None
        self.output: deque[str] = deque(maxlen=500)
        self.session: dict = {"state": "idle", "exit_code": None, "command": []}
        self.preview = PreviewPipeline()

    @property
    def labels_path(self) -> Path:
        return self.results_dir / LABELS_FILE

    def load_labels(self) -> dict:
        try:
            data = json.loads(self.labels_path.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, json.JSONDecodeError):
            return {}

    def save_label(self, relative_path: str, mode: str | None, condition: str | None,
                   group: str = "") -> None:
        target = (self.results_dir / relative_path).resolve()
        if self.results_dir not in target.parents or not target.is_file():
            raise ValueError("run file is outside the results directory")
        labels = self.load_labels()
        key = relative_path.replace("\\", "/")
        existing = labels.get(key, {})
        entry: dict = {}
        # mode/condition are no longer operator-editable — the receiver records
        # them in the NDJSON itself. None means "leave whatever a previous label
        # set", so overrides applied to older logs are not silently discarded.
        for field, value in (("mode", mode), ("condition", condition)):
            chosen = value if value is not None else existing.get(field)
            if chosen:
                entry[field] = chosen
        # Only stored when set, so clearing the field returns the run to being
        # grouped by its real capture mode.
        if group:
            entry["group"] = group
        labels[key] = entry
        temp = self.labels_path.with_suffix(".tmp")
        temp.write_text(json.dumps(labels, indent=2), encoding="utf-8")
        temp.replace(self.labels_path)

    def start_receiver(self, request: dict) -> dict:
        if not RECEIVER_EXE.is_file():
            raise ValueError("receiver binary missing; run experiment/build-receiver.ps1")

        phone = str(ipaddress.ip_address(str(request.get("phone", ""))))
        run_id = str(request.get("run_id", "")).strip()
        if not run_id:
            run_id = f"{int(time.time() * 1000)}-run"
        # The Android harness currently generates "<epoch>-run". Accepting the
        # visible numeric portion is convenient, but the receiver must hash the
        # exact same string or every video packet is correctly rejected.
        if run_id.isdigit():
            run_id += "-run"
        mode = str(request.get("mode", "")).strip().lower()
        condition = str(request.get("condition", "clean")).strip().lower()
        if not SAFE_TEXT.fullmatch(run_id):
            raise ValueError("run_id contains unsupported characters")
        if not MODE_TEXT.fullmatch(mode):
            raise ValueError("mode must look like 320x240@20")
        if not SAFE_TEXT.fullmatch(condition):
            raise ValueError("condition contains unsupported characters")

        duration = max(10, min(3600, int(request.get("duration", 60))))
        warmup = max(0, min(duration - 1, int(request.get("warmup", 5))))
        protocol = str(request.get("protocol", "raw_udp"))
        if protocol not in ("raw_udp", "rtp_udp", "srt", "mjpeg"):
            raise ValueError("protocol belum diimplementasikan")

        with self.lock:
            if self.process and self.process.poll() is None:
                raise ValueError("a receiver session is already running")

            # Never truncate a previous measurement. The UI echoes the
            # generated ID while a run is active, so without this guard a
            # second Start reused that value and NdjsonWriter opened the same
            # receiver-<id>.ndjson with "wb". Keep an explicitly supplied base
            # readable and append a deterministic suffix on collision.
            run_id = unique_run_id(self.results_dir, run_id)

            preview_port = self.preview.start(protocol)
            command = [
                str(RECEIVER_EXE), "--phone", phone,
                "--protocol", protocol,
                "--run-id", run_id,
                "--duration", str(duration),
                "--warmup", str(warmup),
                "--mode", mode,
                "--condition", condition,
                "--preview-port", str(preview_port),
                "--out", str(self.results_dir),
            ]
            self.output.clear()
            self.output.append("Starting receiver...")
            self.session = {
                "state": "starting", "exit_code": None, "command": command,
                "run_id": run_id, "phone": phone, "mode": mode,
                "condition": condition,
            }
            flags = 0
            startupinfo = None
            if os.name == "nt":
                # NEW_PROCESS_GROUP makes the receiver addressable by
                # CTRL_BREAK_EVENT, which its console handler turns into a clean
                # shutdown that writes the summary line. Without the group the
                # event would hit this dashboard too.
                #
                # CREATE_NO_WINDOW must NOT be combined with it: a process with
                # no console receives no console control events at all, so the
                # stop request was silently dropped and the receiver had to be
                # hard killed — producing a log with no summary, permanently
                # stuck at "RUNNING" and never countable as valid. Verified both
                # ways. The window is hidden through STARTUPINFO instead, which
                # keeps a console attached for the signal to arrive on.
                flags = subprocess.CREATE_NEW_PROCESS_GROUP
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = subprocess.SW_HIDE
            self.process = subprocess.Popen(
                command, cwd=REPO_DIR, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                errors="replace", bufsize=1, creationflags=flags,
                startupinfo=startupinfo,
            )
            threading.Thread(target=self._pump_output, daemon=True).start()
            return dict(self.session)

    def _pump_output(self) -> None:
        assert self.process is not None
        process = self.process
        assert process.stdout is not None
        with self.lock:
            self.session["state"] = "running"
        for line in process.stdout:
            with self.lock:
                self.output.append(line.rstrip())
        code = process.wait()
        self.preview.stop()
        with self.lock:
            self.session["state"] = "complete" if code == 0 else "failed"
            self.session["exit_code"] = code

    def stop_receiver(self) -> None:
        with self.lock:
            process = self.process
            phone = str(self.session.get("phone", ""))
        if process and process.poll() is None:
            if phone:
                stop_phone_pipeline(phone)
            # Ask for a graceful stop first. terminate() is TerminateProcess on
            # Windows, which delivers no signal at all — the receiver then dies
            # before writing its summary line, and a run with no summary is
            # permanently stuck at clock_status "RUNNING" and can never be
            # counted as valid. The receiver's handler turns CTRL_BREAK into
            # g_quit and exits through the normal summary path.
            graceful = False
            if os.name == "nt":
                try:
                    process.send_signal(signal.CTRL_BREAK_EVENT)
                    graceful = True
                except (OSError, ValueError, AttributeError):
                    graceful = False
            else:
                process.terminate()  # SIGTERM is already handled there
                graceful = True
            if graceful:
                try:
                    process.wait(timeout=10)
                    return
                except subprocess.TimeoutExpired:
                    pass  # fall through to the hard kill below
            process.kill()

    def session_snapshot(self) -> dict:
        with self.lock:
            data = dict(self.session)
            data["output"] = list(self.output)
            data["preview"] = self.preview.snapshot()
            return data


def parse_run(path: Path, root: Path, labels: dict, include_series: bool) -> dict:
    meta: dict = {}
    summary: dict = {}
    control: dict = {}
    frames: list[dict] = []
    parse_errors = 0
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    parse_errors += 1
                    continue
                kind = record.get("type")
                if kind == "meta":
                    meta = record
                elif kind == "frame":
                    frames.append(record)
                elif kind == "control":
                    control = record
                elif kind == "summary":
                    summary = record
    except OSError as exc:
        return {"file": str(path), "error": str(exc), "valid": False}

    relative = path.relative_to(root).as_posix()
    label = labels.get(relative, {})
    mode = label.get("mode") or meta.get("mode") or "unknown"
    condition = label.get("condition") or meta.get("condition") or "unspecified"
    offset = int(meta.get("clock_offset_ns", 0))
    transport: list[float] = []
    glass: list[float] = []
    encode: list[float] = []
    series: list[dict] = []
    jitter_ns = 0.0
    previous: tuple[int, int] | None = None
    previous_seq: int | None = None
    running_gaps = 0
    running_received = 0
    protocol = meta.get("protocol", "unknown")

    for index, frame in enumerate(frames):
        cap = int(frame["cap_ns"])
        enc = int(frame["enc_ns"])
        snd = int(frame["snd_ns"])
        rcv = int(frame["rcv_ns"])
        trans_ms = (rcv + offset - snd) / 1e6
        glass_ms = (rcv + offset - cap) / 1e6
        enc_ms = (enc - cap) / 1e6
        transport.append(trans_ms)
        glass.append(glass_ms)
        encode.append(enc_ms)
        if previous is not None:
            d = abs((rcv - previous[1]) - (snd - previous[0]))
            jitter_ns += (d - jitter_ns) / 16.0
        previous = (snd, rcv)
        seq_value = int(frame.get("seq", 0))
        if previous_seq is not None:
            delta = (seq_value - previous_seq) & 0xFFFFFFFF
            if 1 < delta < 0x80000000:
                running_gaps += delta - 1
        previous_seq = seq_value
        running_received += 1
        packet_received = int(frame.get("packets_received", 0))
        packet_lost = int(frame.get("packets_lost", 0))
        if protocol in ("raw_udp", "rtp_udp", "srt", "mjpeg") and packet_received + packet_lost:
            running_loss = 100.0 * packet_lost / (packet_received + packet_lost)
        else:
            running_loss = 100.0 * running_gaps / (running_received + running_gaps)
        if include_series:
            series.append({
                "i": index + 1, "seq": frame.get("seq", 0),
                # Seconds since the run's first received frame. Derived from
                # rcv_ns (PC clock) so it needs no clock-offset correction, and
                # measured against the first frame rather than the log's own
                # start so warmup discards do not shift the axis.
                "t": round((rcv - int(frames[0]["rcv_ns"])) / 1e9, 3),
                "latency": round(trans_ms, 4),
                "jitter": round(jitter_ns / 1e6, 4),
                "loss": round(running_loss, 5),
                "bytes": frame.get("bytes", 0),
            })

    duration_s = 0.0
    if len(frames) > 1:
        duration_s = (int(frames[-1]["rcv_ns"]) - int(frames[0]["rcv_ns"])) / 1e9
    payload = int(summary.get("bytes_payload", sum(int(f.get("bytes", 0)) for f in frames)))
    wire = int(summary.get("bytes_wire", payload))
    received = int(summary.get("frames_received", len(frames)))
    gaps = int(summary.get("gap_frames", running_gaps))
    total_expected = received + gaps
    packets_received = int(summary.get(
        "packets_received", frames[-1].get("packets_received", 0) if frames else 0))
    packets_lost = int(summary.get(
        "packets_lost", frames[-1].get("packets_lost", 0) if frames else 0))
    packets_recovered = int(summary.get(
        "packets_recovered", frames[-1].get("packets_recovered", 0) if frames else 0))
    if protocol == "raw_udp" and packets_received + packets_lost:
        loss_pct = 100.0 * packets_lost / (packets_received + packets_lost)
        loss_basis = "udp_packets"
    elif protocol == "rtp_udp" and packets_received + packets_lost:
        loss_pct = 100.0 * packets_lost / (packets_received + packets_lost)
        loss_basis = "rtp_packets"
    elif protocol == "srt" and packets_received + packets_lost:
        loss_pct = 100.0 * packets_lost / (packets_received + packets_lost)
        loss_basis = "srt_detected_packets"
    elif protocol == "mjpeg" and packets_received + packets_lost:
        loss_pct = 100.0 * packets_lost / (packets_received + packets_lost)
        loss_basis = "tcp_retransmissions"
    else:
        loss_pct = 100.0 * gaps / total_expected if total_expected else 0.0
        loss_basis = "frames"
    clock_status = summary.get("clock_status", "RUNNING" if frames else "INCOMPLETE")

    result = {
        "file": relative,
        "run_id": meta.get("run_id", path.stem.removeprefix("receiver-")),
        "protocol": protocol,
        "mode": mode,
        "condition": condition,
        # Operator-chosen comparison bucket. Empty means "group by capture mode".
        "group": str(label.get("group", "")),
        "modified": datetime.fromtimestamp(path.stat().st_mtime).isoformat(timespec="seconds"),
        "valid": clock_status == "OK" and bool(frames),
        "clock_status": clock_status,
        "clock_drift_ms": round(int(summary.get("clock_drift_ns", 0)) / 1e6, 3),
        "frames": len(frames),
        "frames_reported": received,
        "fps": round(len(frames) / duration_s, 2) if duration_s > 0 else 0.0,
        "transport": {
            "p50": round(percentile(transport, 0.50), 3),
            "p95": round(percentile(transport, 0.95), 3),
            "p99": round(percentile(transport, 0.99), 3),
            "max": round(max(transport), 3) if transport else 0.0,
        },
        "glass": {
            "p50": round(percentile(glass, 0.50), 3),
            "p95": round(percentile(glass, 0.95), 3),
            "p99": round(percentile(glass, 0.99), 3),
        },
        "encode": {
            "p50": round(percentile(encode, 0.50), 3),
            "p95": round(percentile(encode, 0.95), 3),
        },
        "jitter_ms": round(jitter_ns / 1e6, 3),
        "gaps": gaps,
        "loss_pct": round(loss_pct, 4),
        "loss_basis": loss_basis,
        "packets_received": packets_received,
        "packets_lost": packets_lost,
        "packets_recovered": packets_recovered,
        "packet_reorder": int(summary.get("packet_reorder_events", 0)),
        "packet_duplicates": int(summary.get("packet_duplicates", 0)),
        "reorder": int(summary.get("reorder_events", 0)),
        "duplicates": int(summary.get("duplicates", 0)),
        "reassembly_failures": int(summary.get("reassembly_failures", 0)),
        "goodput_mbps": round(8.0 * payload / duration_s / 1e6, 3) if duration_s > 0 else 0.0,
        "overhead_pct": round(100.0 * (wire - payload) / payload, 3) if payload else 0.0,
        "control": control,
        "parse_errors": parse_errors,
    }
    if include_series:
        result["series"] = downsample(series)
    return result


class Handler(BaseHTTPRequestHandler):
    server_version = "UcvDashboard/1.0"

    @property
    def state(self) -> DashboardState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args) -> None:
        print(f"[dashboard] {self.address_string()} {fmt % args}")

    def send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        payload = json.dumps(value, separators=(",", ":"), allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            payload = (DASHBOARD_DIR / "index.html").read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if parsed.path == "/api/session":
            self.send_json(self.state.session_snapshot())
            return
        if parsed.path == "/api/modes":
            try:
                phone = parse_qs(parsed.query).get("phone", [""])[0]
                advertised = query_camera_modes(phone)
                resolutions = group_camera_modes(advertised)
                preferred = "320x240@20" if "320x240@20" in advertised else advertised[0]
                self.send_json({"resolutions": resolutions,
                                "recommended": preferred,
                                "advertised_count": len(advertised),
                                "format": "MJPEG"})
            except ValueError as exc:
                self.send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        if parsed.path == "/api/preview.mjpeg":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.end_headers()
            sequence = -1
            try:
                while True:
                    next_sequence, jpeg = self.state.preview.wait_frame(sequence)
                    if next_sequence == sequence:
                        continue
                    sequence = next_sequence
                    if jpeg is None:
                        continue
                    self.wfile.write(
                        b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                        + str(len(jpeg)).encode("ascii") + b"\r\n\r\n"
                        + jpeg + b"\r\n"
                    )
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            return
        if parsed.path in ("/api/runs", "/api/run"):
            labels = self.state.load_labels()
            if parsed.path == "/api/run":
                relative = parse_qs(parsed.query).get("file", [""])[0]
                target = (self.state.results_dir / relative).resolve()
                if self.state.results_dir not in target.parents or not target.is_file():
                    self.send_json({"error": "run not found"}, HTTPStatus.NOT_FOUND)
                    return
                self.send_json(parse_run(target, self.state.results_dir, labels, True))
                return
            runs = [
                parse_run(path, self.state.results_dir, labels, False)
                for path in self.state.results_dir.rglob("receiver-*.ndjson")
            ]
            runs.sort(key=lambda item: item.get("modified", ""), reverse=True)
            self.send_json({"runs": runs, "results_dir": str(self.state.results_dir)})
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 64 * 1024:
            raise ValueError("invalid request body")
        value = json.loads(self.rfile.read(length))
        if not isinstance(value, dict):
            raise ValueError("JSON object required")
        return value

    def do_POST(self) -> None:
        try:
            request = self.read_json()
            if self.path == "/api/start":
                self.send_json(self.state.start_receiver(request), HTTPStatus.ACCEPTED)
                return
            if self.path == "/api/stop":
                self.state.stop_receiver()
                self.send_json({"ok": True})
                return
            if self.path == "/api/label":
                # Absent (not merely empty) means "keep what is already stored".
                mode = request.get("mode")
                mode = str(mode).strip().lower() if mode is not None else None
                condition = request.get("condition")
                condition = str(condition).strip().lower() if condition is not None else None
                group = " ".join(str(request.get("group", "")).split())
                if mode not in (None, "unknown") and not MODE_TEXT.fullmatch(mode):
                    raise ValueError("mode must look like 320x240@20")
                if condition is not None and not SAFE_TEXT.fullmatch(condition):
                    raise ValueError("invalid condition")
                if group and not GROUP_TEXT.fullmatch(group):
                    raise ValueError("group may use letters, numbers, spaces and _.@:+/()-")
                self.state.save_label(str(request.get("file", "")), mode, condition, group)
                self.send_json({"ok": True})
                return
            self.send_error(HTTPStatus.NOT_FOUND)
        except (ValueError, json.JSONDecodeError) as exc:
            self.send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    state = DashboardState(args.results)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.state = state  # type: ignore[attr-defined]
    url = f"http://{args.host}:{args.port}/"
    print(f"UCV dashboard: {url}")
    print(f"Results: {state.results_dir}")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.stop_receiver()
        state.preview.stop()
        server.server_close()


if __name__ == "__main__":
    main()
