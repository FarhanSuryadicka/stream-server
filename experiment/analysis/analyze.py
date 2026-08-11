#!/usr/bin/env python3
"""Turn receiver NDJSON logs into the comparison table of harness spec section 8.

Metric formulas live in experiment/docs/01-harness-spec.md section 5 and are
reimplemented here from raw timestamps rather than reading derived values out
of the logs. That is deliberate: the logs record raw timestamps only, so a
corrected formula can be re-applied to historical runs without re-measuring.

Usage:
    python analyze.py results/*.ndjson --out summary.md
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path


def percentile(sorted_vals: list[float], p: float) -> float:
    """Linear-interpolated percentile. Mirrors ucv::Distribution::Percentile
    in the C++ receiver so both report identical numbers."""
    if not sorted_vals:
        return 0.0
    if p <= 0:
        return sorted_vals[0]
    if p >= 1:
        return sorted_vals[-1]
    idx = p * (len(sorted_vals) - 1)
    lo, hi = math.floor(idx), math.ceil(idx)
    if lo == hi:
        return sorted_vals[lo]
    frac = idx - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


@dataclass
class Run:
    path: Path
    run_id: str = ""
    protocol: str = ""
    mode: str = "unknown"
    condition: str = "unspecified"
    clock_offset_ns: int = 0
    clock_status: str = "UNKNOWN"
    clock_drift_ns: int = 0

    transport_ms: list[float] = field(default_factory=list)
    glass_ms: list[float] = field(default_factory=list)
    encode_ms: list[float] = field(default_factory=list)

    frames_received: int = 0
    gap_frames: int = 0
    reorder_events: int = 0
    duplicates: int = 0
    reassembly_failures: int = 0
    bytes_payload: int = 0
    bytes_wire: int = 0

    duration_s: float = 0.0
    jitter_ms: float = 0.0

    @property
    def valid(self) -> bool:
        return self.clock_status == "OK" and len(self.transport_ms) > 0

    @property
    def loss_pct(self) -> float:
        """Loss observed from sequence gaps.

        NOTE: this is a LOWER BOUND. A tail-end loss leaves no gap and is
        invisible here; the true figure needs the sender's own frames_sent
        (harness spec section 5.5). Reported as such rather than as the
        authoritative number.
        """
        total = self.frames_received + self.gap_frames
        return 100.0 * self.gap_frames / total if total else 0.0

    @property
    def goodput_mbps(self) -> float:
        if self.duration_s <= 0:
            return 0.0
        return 8.0 * self.bytes_payload / self.duration_s / 1e6

    @property
    def overhead_pct(self) -> float:
        if self.bytes_payload <= 0:
            return 0.0
        return 100.0 * (self.bytes_wire - self.bytes_payload) / self.bytes_payload


def load_run(path: Path) -> Run | None:
    run = Run(path=path)
    frames: list[dict] = []

    try:
        with path.open("r", encoding="utf-8") as f:
            for line_no, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    # A truncated last line is expected if a run was killed;
                    # anything else is worth flagging rather than swallowing.
                    print(f"  warn: {path.name}:{line_no} unparseable, skipped",
                          file=sys.stderr)
                    continue

                t = rec.get("type")
                if t == "meta":
                    run.run_id = rec.get("run_id", "")
                    run.protocol = rec.get("protocol", "")
                    run.mode = rec.get("mode", "unknown")
                    run.condition = rec.get("condition", "unspecified")
                    run.clock_offset_ns = rec.get("clock_offset_ns", 0)
                elif t == "frame":
                    frames.append(rec)
                elif t == "summary":
                    run.frames_received = rec.get("frames_received", 0)
                    run.gap_frames = rec.get("gap_frames", 0)
                    run.reorder_events = rec.get("reorder_events", 0)
                    run.duplicates = rec.get("duplicates", 0)
                    run.reassembly_failures = rec.get("reassembly_failures", 0)
                    run.bytes_payload = rec.get("bytes_payload", 0)
                    run.bytes_wire = rec.get("bytes_wire", 0)
                    run.clock_status = rec.get("clock_status", "UNKNOWN")
                    run.clock_drift_ns = rec.get("clock_drift_ns", 0)
    except OSError as e:
        print(f"  error: cannot read {path}: {e}", file=sys.stderr)
        return None

    if not frames:
        return run

    off = run.clock_offset_ns
    prev_snd = prev_rcv = None
    jitter = 0.0

    for fr in frames:
        cap, enc = fr["cap_ns"], fr["enc_ns"]
        snd, rcv = fr["snd_ns"], fr["rcv_ns"]

        # Express the PC arrival time on the phone timebase before
        # subtracting — the entire reason clock sync exists.
        run.transport_ms.append((rcv + off - snd) / 1e6)
        run.glass_ms.append((rcv + off - cap) / 1e6)
        run.encode_ms.append((enc - cap) / 1e6)

        # RFC 3550: difference of differences, so the clock offset cancels.
        if prev_snd is not None:
            d = abs((rcv - prev_rcv) - (snd - prev_snd))
            jitter += (d - jitter) / 16.0
        prev_snd, prev_rcv = snd, rcv

    run.jitter_ms = jitter / 1e6
    span_ns = frames[-1]["rcv_ns"] - frames[0]["rcv_ns"]
    run.duration_s = span_ns / 1e9 if span_ns > 0 else 0.0
    return run


def summarise(runs: list[Run]) -> str:
    by_proto: dict[str, list[Run]] = {}
    for r in runs:
        by_proto.setdefault(r.protocol or "unknown", []).append(r)

    out: list[str] = []
    out.append("# Protocol Comparison — Measured Results\n")
    out.append(f"Runs analysed: **{len(runs)}**  "
               f"(valid: {sum(1 for r in runs if r.valid)}, "
               f"excluded: {sum(1 for r in runs if not r.valid)})\n")

    invalid = [r for r in runs if not r.valid]
    if invalid:
        out.append("\n## Excluded runs\n")
        out.append("Excluded rather than averaged in — a run whose clock sync "
                   "drifted reports latency that is partly clock offset.\n")
        out.append("\n| run_id | protocol | reason |")
        out.append("|---|---|---|")
        for r in invalid:
            reason = (f"clock {r.clock_status} (drift {r.clock_drift_ns/1e6:.2f} ms)"
                      if r.clock_status != "OK" else "no frames recorded")
            out.append(f"| `{r.run_id or r.path.name}` | {r.protocol or '?'} | {reason} |")
        out.append("")

    out.append("\n## Headline comparison\n")
    out.append("Transport latency is the ranking metric (harness spec 5.1): it "
               "excludes capture and encode, which are identical across runs.\n")
    out.append("\n| Protocol | n | Transport p50 | p95 | p99 | Jitter | Loss%* | Goodput | Overhead |")
    out.append("|---|---|---|---|---|---|---|---|---|")

    for proto in sorted(by_proto):
        valid = [r for r in by_proto[proto] if r.valid]
        if not valid:
            out.append(f"| {proto} | 0 | — | — | — | — | — | — | — |")
            continue
        allt = sorted(v for r in valid for v in r.transport_ms)
        out.append(
            f"| {proto} | {len(valid)} "
            f"| {percentile(allt, 0.50):.2f} ms "
            f"| {percentile(allt, 0.95):.2f} ms "
            f"| {percentile(allt, 0.99):.2f} ms "
            f"| {statistics.median(r.jitter_ms for r in valid):.2f} ms "
            f"| {statistics.median(r.loss_pct for r in valid):.2f} "
            f"| {statistics.median(r.goodput_mbps for r in valid):.2f} Mbps "
            f"| {statistics.median(r.overhead_pct for r in valid):.1f}% |")

    out.append("\n\\* Loss from sequence gaps only — a **lower bound**. "
               "A tail-end loss leaves no gap; the true figure needs the "
               "sender's own frames_sent count.\n")

    out.append("\n## Glass-to-glass (product-facing)\n")
    out.append("Includes capture, encode, transport. A poor protocol "
               "discriminator — the constant encode cost compresses the "
               "differences — but it is what a human perceives.\n")
    out.append("\n| Protocol | p50 | p95 | p99 | capture→encode p50 |")
    out.append("|---|---|---|---|---|")
    for proto in sorted(by_proto):
        valid = [r for r in by_proto[proto] if r.valid]
        if not valid:
            continue
        allg = sorted(v for r in valid for v in r.glass_ms)
        alle = sorted(v for r in valid for v in r.encode_ms)
        out.append(f"| {proto} "
                   f"| {percentile(allg, 0.50):.2f} ms "
                   f"| {percentile(allg, 0.95):.2f} ms "
                   f"| {percentile(allg, 0.99):.2f} ms "
                   f"| {percentile(alle, 0.50):.2f} ms |")

    out.append("\n## Reliability\n")
    out.append("\n| Protocol | frames | gaps | reorder | dup | reassembly fail |")
    out.append("|---|---|---|---|---|---|")
    for proto in sorted(by_proto):
        valid = [r for r in by_proto[proto] if r.valid]
        if not valid:
            continue
        out.append(f"| {proto} "
                   f"| {sum(r.frames_received for r in valid)} "
                   f"| {sum(r.gap_frames for r in valid)} "
                   f"| {sum(r.reorder_events for r in valid)} "
                   f"| {sum(r.duplicates for r in valid)} "
                   f"| {sum(r.reassembly_failures for r in valid)} |")

    out.append("\n## Per-run detail\n")
    out.append("\n| run_id | protocol | n frames | transport p50/p95/p99 | jitter | clock |")
    out.append("|---|---|---|---|---|---|")
    for r in sorted(runs, key=lambda x: (x.protocol, x.run_id)):
        if not r.transport_ms:
            out.append(f"| `{r.run_id or r.path.name}` | {r.protocol} | 0 | — | — | {r.clock_status} |")
            continue
        t = sorted(r.transport_ms)
        out.append(f"| `{r.run_id}` | {r.protocol} | {len(t)} "
                   f"| {percentile(t,0.50):.2f} / {percentile(t,0.95):.2f} / {percentile(t,0.99):.2f} ms "
                   f"| {r.jitter_ms:.2f} ms | {r.clock_status} |")

    out.append("\n## How to read this\n")
    out.append("- **Differences below ~5 ms are not resolvable.** Clock sync "
               "carries ±1–3 ms residual error (harness spec 4.2). Two "
               "protocols within 5 ms are tied, not ranked.\n")
    out.append("- **Rank on p95/p99, not the mean.** A 40 ms mean with a 400 ms "
               "p99 is worse for real-time control than 60 ms mean with 70 ms p99.\n")
    out.append("- **Jitter is trustworthy even if clock sync is not** — RFC 3550's "
               "difference-of-differences cancels the offset.\n")
    out.append("- **MJPEG bandwidth is not comparable** to the H.264 protocols: "
               "it carries JPEG with no inter-frame compression. Its latency and "
               "control numbers are comparable.\n")
    out.append("- **Overhead here counts application bytes only.** Retransmissions "
               "and header overhead need a packet capture (harness spec 5.7).\n")
    out.append("- **This table does not decide the winner on its own.** The "
               "upstream control channel is a first-class criterion; a protocol "
               "that streams well but cannot carry a low-latency back-channel "
               "has not won.\n")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="receiver-*.ndjson files (globs ok)")
    ap.add_argument("--out", help="write markdown here (default: stdout)")
    args = ap.parse_args()

    paths: list[Path] = []
    for pattern in args.logs:
        matched = [Path(p) for p in glob.glob(pattern)]
        if not matched:
            print(f"warn: no files matched '{pattern}'", file=sys.stderr)
        paths.extend(matched)

    if not paths:
        print("error: no log files found", file=sys.stderr)
        return 1

    runs = [r for r in (load_run(p) for p in sorted(set(paths))) if r is not None]
    if not runs:
        print("error: no runs could be loaded", file=sys.stderr)
        return 1

    report = summarise(runs)
    if args.out:
        Path(args.out).write_text(report, encoding="utf-8")
        print(f"wrote {args.out}  ({len(runs)} run(s))")
    else:
        # The Windows console defaults to cp1252, which cannot encode the
        # arrows and multiplication signs used in the report. Write bytes
        # directly as UTF-8 rather than losing characters or crashing.
        sys.stdout.buffer.write(report.encode("utf-8"))
        sys.stdout.buffer.write(b"\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
