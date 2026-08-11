# AGENTS.md

Android app (`:app`, single module, Kotlin + Compose) that turns the phone into an HTTP MJPEG server for a USB UVC camera — raw passthrough, no dewarp, no re-encode. The real logic lives in native C; Kotlin is a thin USB-permission + control UI glue.

`CLAUDE.md` is a near-duplicate for Claude Code sessions — keep the two in sync when editing either.

## Build

- Windows: `.\gradlew.bat :app:assembleDebug` (or `installDebug`, `installRelease`, `:app:lint`, `:app:testDebugUnitTest`).
- Versions in `gradle/libs.versions.toml`: Gradle 9.5.0 wrapper, AGP 9.3.1, Kotlin 2.2.10. Toolchain pinned: NDK `27.1.12297006`, CMake `3.22.1`, compileSdk 36 (minorApiLevel 1), minSdk 24.
- `abiFilters` = **arm64-v8a only** — native lib won't build for x86/emulator until an ABI is added.
- Release build is signed with the **debug keystore** (no real signing config) so `installRelease` works for local perf testing — not for distribution. R8/optimization disabled.
- Tests are template placeholders only (`ExampleUnitTest`, `ExampleInstrumentedTest`), no real coverage exists. Native code has no test harness (needs a device + camera). Configuration cache is on.
- No third-party deps beyond AndroidX/Compose.

## Native architecture

- Entry: `app/src/main/cpp/CMakeLists.txt` + `jni_bridge.c` (~450 lines, all native logic). Kotlin side is `UvcNative.kt` (JNI surface) + `MainActivity.kt`.
- Everything else under `app/src/main/cpp/` is **vendored upstream, committed in-tree, not git submodules** (`libjpeg-turbo`, `libuvc`, `libusb-cmake`). Only `CMakeLists.txt` and `jni_bridge.c` are project files. Don't edit vendored sources casually; `libusb-cmake` even contains a stray embedded `.git` dir. libjpeg-turbo is vendored/built but currently **unused** — no JPEG en/decode happens in this pipeline, frames are forwarded byte-for-byte.
- JNI symbols are `Java_com_anjas_uvcserver_UvcNative_*`; the lib is `libuvcserver.so`. Renaming the package/object/function in Kotlin **requires** updating `jni_bridge.c` names.

## The pipeline: capture → HTTP multipart passthrough

1. **Capture thread** — `uvc_stream_get_frame` blocks for the next MJPEG frame off the USB stream, memcpy's it into a shared `frame_state_t` buffer under a mutex, bumps a sequence counter, and `pthread_cond_broadcast`s.
2. **Accept thread** — plain `accept()` loop on a raw POSIX socket (`socket`/`bind`/`listen`), spawns a detached `client_thread` per connection.
3. **Client thread** — sends the `multipart/x-mixed-replace; boundary=frame` HTTP header once, then loops: wait on the frame's condvar for a new sequence number, write a `--frame` part with the raw JPEG bytes. No decode, no re-encode — whatever the camera emitted is what the client gets. Any browser or `<img src="http://phone-ip:8181/">` can view it directly.
4. **Stop** — `stopServer` sets `g_quit`, shuts down the listen socket (unblocks `accept`), joins capture+accept threads, then stops/closes the UVC stream.

## Debugging

- The phone's USB port is occupied by the camera, so `adb logcat` over USB is unavailable while streaming. Native code keeps a 200-line in-app ring buffer polled by the UI every 500 ms via `getLogText()` — the on-screen log is the primary debug surface. `LOGI`/`LOGE` mirror to both logcat and the ring buffer.
- Wiring up a USB camera on this app is: plug camera → grant CAMERA/RECORD_AUDIO runtime permission → USB permission → open fd → native `uvc_wrap(fd)`. Needs a real arm64 device; emulators have no USB host.
- Some OEM ROMs (MIUI/HyperOS observed) gate any USB device declaring a Video-class interface behind the same CAMERA/RECORD_AUDIO runtime permissions Camera2 needs, even though this app only does raw USB host I/O — `MainActivity.requestCameraAccessWithPermissions()` requests both before opening the device.
- Gradle's CMake incremental build has been observed serving a stale `.so` (old JNI symbols) after editing `jni_bridge.c` even though the task reports as executed, not UP-TO-DATE. If a JNI call throws `UnsatisfiedLinkError` for a symbol you just added, `.\gradlew.bat :app:clean` before rebuilding — don't assume the C code is wrong first.
- If `uvc_stream_get_frame` negotiates a mode successfully but never actually returns a frame (no error logged, no crash, just blocks forever), that's an isochronous USB bandwidth stall, not a code bug — try a lower resolution/fps. `startServer` logs every mode the camera actually offers (`mode[i]: WxH@FPSfps`) before picking one; use that list to find a resolution known to work on the attached camera rather than guessing.

## Gotchas

- Native mode selection picks the "highest score" (max res) camera mode, which corrupts or silently stalls over isochronous USB. `MainActivity.kt` pins `CAP_W/H/FPS` = `1280x720@30` as a conservative default — raise resolution only after confirming clean frames on-device (check `mode[i]` log lines for what the camera actually offers).
- UVC detection matches by interface class (`USB_CLASS_VIDEO`) since composite cameras declare Video only on one interface — don't "simplify" to device-level checks (`MainActivity.findUvcDevices()`).
- MJPEG input only (no YUYV) — `collect_modes` only enumerates `UVC_VS_FORMAT_MJPEG` descriptors.
- Only one server instance at a time — `startServer` returns `false` if `g_server_running` is already set; always route through `stopServer()` before starting again.
