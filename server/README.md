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

It covers the same ground as the web dashboard: start/stop a measurement, choose
the camera's advertised MJPEG resolution, live camera preview, live
latency/jitter/loss charts, run history, and manual comparison groups. Both
front ends read the same NDJSON and share `.dashboard-labels.json`, so a group
labelled in one shows up in the other.

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

## Live preview

Same pipeline as the web dashboard, for the same reason: the receiver is the
only listener on the video port, so a second socket competing for the phone's
packets could corrupt the measurement. The receiver mirrors each complete frame
to a localhost UDP port *after* reception, and the preview reads from there —
it therefore cannot affect a single number in the run.

H.264 protocols are decoded by FFmpeg (h264 to mjpeg over stdio) with the same
arguments the Python dashboard uses, so neither preview is "the good one".
MJPEG already carries JPEG and is passed through untouched, which is why its
preview works with no FFmpeg installed at all.

## Camera modes

"Refresh camera modes" asks the phone's control agent (UDP 8200) to enumerate
what the camera actually advertises, over the shared wire contract in
`experiment/receiver/include/ucv_wire.h`. Offering a fixed list instead would
let an operator pick a mode the camera does not have, and that fails at START
with an error indistinguishable from a network fault.

The query runs off the UI thread: enumerating up to 256 indices with a 600 ms
timeout each would otherwise freeze the window for minutes when the phone is not
answering.

## Build

Requires Qt 6.5+ and CMake 3.21+. Qt is **not** vendored — it is a large
external SDK and `FUTURE-SERVER-ARCHITECTURE.md` §6 explicitly warns against
adding it to the tree speculatively.

### The short version

Everything is native Windows — **no WSL, no MSYS2**. Two scripts wrap the CMake
invocations below:

```powershell
.\server\build.ps1              # build + test the full app
.\server\run.ps1                # launch it
```

Useful switches:

| Command | What it does |
|---|---|
| `.\server\build.ps1 -CoreOnly` | measurement logic + tests only, no Qt required |
| `.\server\build.ps1 -Clean` | delete the build directory first (see the warning below) |
| `.\server\build.ps1 -Run` | launch as soon as the build succeeds |
| `.\server\run.ps1 -Console` | show Qt/QML log output, for when something misbehaves |

The rest of this section explains what those scripts do, and is worth reading
once because two of the details are non-obvious and cost a long debugging
session to find.

### Core only — no Qt needed

Useful on any machine, and the fastest way to check that the measurement logic
is intact:

```powershell
cmake -S server -B server/build-core -DUCV_BUILD_UI=OFF
cmake --build server/build-core
ctest --test-dir server/build-core --output-on-failure
```

### Full application

**Use the MinGW that built your Qt.** This is not optional on Windows and cost
a long debugging session to learn: Qt's prebuilt binaries ship their own
`libstdc++-6.dll`, and Qt6Core imports from it. A newer GCC links fine and then
the application fails to *start* with

```text
The procedure entry point _ZNKSt25__codecvt_utf8_utf16... could not be located
```

Nothing in the build log hints at it, and static-linking the runtime does not
help, because the missing symbol is imported by Qt's DLLs rather than by this
binary. `CMakeLists.txt` warns when it detects a likely mismatch.

Qt 6.8.1 is built with GCC 13.1.0. If you installed Qt with `aqtinstall`, the
matching toolchain is one command away:

```powershell
pip install aqtinstall
python -m aqt install-qt   windows desktop 6.8.1 win64_mingw -O C:/Qt
python -m aqt install-tool windows desktop tools_mingw1310    -O C:/Qt
```

```powershell
cmake -S server -B server/build -G "MinGW Makefiles" `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/mingw_64" `
      -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
      -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe" `
      -DCMAKE_MAKE_PROGRAM="C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe"
cmake --build server/build -j 8
```

CMake caches the compiler, so **after changing toolchain or Qt version, delete
the build directory** rather than reconfiguring in place — a stale cache silently
keeps building with the old compiler and produces a binary that fails only at
startup:

```powershell
Remove-Item -Recurse -Force server/build
```

## Running

Needs `ucv-receiver.exe` built first (`experiment/build-receiver.ps1`). The app
finds it relative to the repository root, exactly as the Python dashboard does.

```powershell
server/build/ui/ucv-monitor.exe
```

The build runs `windeployqt`, so the Qt runtime sits beside the executable and
it launches by double-click with no PATH setup. That matters for more than
convenience: Git for Windows ships a conflicting `libstdc++-6.dll` on a
directory that is on nearly every PATH, and Windows searches the executable's
own directory first.
