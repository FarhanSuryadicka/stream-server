# UCV Monitor — desktop control-room application (Qt 6 / C++20)

A **native desktop alternative** to the Python/HTML research dashboard. Both
read the same NDJSON logs and drive the same `ucv-receiver`, so you can run
either one — or neither — without affecting the other.

> The Python dashboard in `experiment/dashboard/` is unchanged and remains the
> reference harness (`FUTURE-SERVER-ARCHITECTURE.md` §1). This application does
> not replace it, remove it, or share state with it beyond the on-disk NDJSON
> files and the receiver binary.

## Why this exists

`docs/FUTURE-SERVER-ARCHITECTURE.md` §6 records C++20 + Qt 6/QML as the
intended production control-room stack. This is the first step in that
direction, deliberately scoped to what the project can actually feed today:
measurement runs and their metrics.

It is **not** the production surveillance server. Multi-room grids, alerts,
recording, SQLite and RBAC (§5) are not implemented, because the data they
would display does not exist yet — there is no AI event source and no
multi-device deployment. The structure below leaves room for them without
requiring a rewrite.

## Structure

Follows the core/UI split in §7, so recording and device sessions can later
survive a UI restart:

```text
server/
  core/                 monitor-core — C++20, NO Qt dependency
    include/ucv/        public headers
    src/                NDJSON parsing, metrics, receiver process control
    tests/              host tests, no display or camera needed
  ui/                   monitor-ui — Qt 6 Widgets/QML front end
  CMakeLists.txt
```

`core` has no Qt include anywhere. That is enforceable and deliberate: it keeps
the measurement logic testable headlessly, and lets a future `monitor-core`
daemon reuse it verbatim.

## Metric parity with the Python dashboard

The two dashboards must not disagree about a run, so the formulas live in one
documented place (`core/src/metrics.cpp`) and mirror `experiment/dashboard/server.py`:

| Metric | Definition |
|---|---|
| transport latency | `rcv_ns + clock_offset_ns - snd_ns` |
| glass-to-glass | `rcv_ns + clock_offset_ns - cap_ns` |
| jitter | RFC 3550 estimator over `(snd, rcv)` pairs |
| loss basis | per protocol — UDP/RTP/SRT packets, TCP retransmits, else frame gaps |
| valid run | `clock_status == "OK"` **and** at least one frame |

`core/tests/parity_test.cpp` asserts these against fixed inputs, so a change to
either dashboard that silently alters a number fails a test rather than
producing two different answers for the same log.

## Build

Requires Qt 6.5+ and CMake 3.21+. Qt is **not** vendored — it is a large
external SDK and `FUTURE-SERVER-ARCHITECTURE.md` §6 explicitly warns against
adding it to the tree speculatively.

```powershell
cmake -S server -B server/build -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/mingw_64"
cmake --build server/build --config Release
```

The core library and its tests build **without** Qt, which is useful on a
machine that has no Qt installed:

```powershell
cmake -S server -B server/build-core -DUCV_BUILD_UI=OFF
cmake --build server/build-core
ctest --test-dir server/build-core --output-on-failure
```

## Running

The application needs `ucv-receiver.exe` built first
(`experiment/build-receiver.ps1`). It locates it relative to the repository
root, the same way the Python dashboard does.

```powershell
server/build/ui/ucv-monitor.exe
```
