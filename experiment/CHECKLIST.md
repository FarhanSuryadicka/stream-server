# Run Checklist — one page

Every time the app is reopened, start again from step 1. Reopening kills the
native process, so the control channel, the `run_id`, and the open camera
handle are all gone with it — nothing carries over.

Full detail: [`docs/04-run-procedure.md`](docs/04-run-procedure.md).
Bring-up troubleshooting: [`docs/03-implementation-plan.md`](docs/03-implementation-plan.md) §11.

---

## Addresses on this rig

| | |
|---|---|
| PC (wired, enter this **on the phone**) | `192.168.0.100` |
| Phone (WiFi, pass this to the **receiver**) | `192.168.0.101` |

The phone's IP can change after a reconnect. Confirm it under
Settings → About phone → Status, or from the router's client list.

---

## Before the run

- [ ] Phone on **mains power**, battery > 50 %
- [ ] **Screen stays on** — Android WiFi power-save otherwise adds a latency
      tail that has nothing to do with the protocol (harness spec §5.4.2)
- [ ] Camera plugged in via OTG
- [ ] Phone on the router's WiFi, PC on the LAN cable

```powershell
.\experiment\Measure-Baseline.ps1 -Phone 192.168.0.101
.\experiment\Test-Connectivity.ps1 -Phone 192.168.0.101
```

Record the baseline line. Control RTT is not interpretable without it.

---

## The run — order matters

### 1. Phone: open the camera

**Open USB Camera** → allow **CAMERA** and **RECORD_AUDIO** → allow **USB**

> HyperOS gates USB video-class devices behind the camera permissions even
> though this app only does raw USB host I/O.

Expect: `UVC device opened (fd=NN)` in the log.

### 2. Phone: start the control channel

Set **PC IP (receiver)** to `192.168.0.100`, then **1. Start Control**.

**Write down the `run_id`** — the receiver needs exactly this string.

> This must be up before the receiver starts. The receiver's first action is a
> clock-sync probe, and it aborts the run if the phone does not answer rather
> than reporting latency that is really clock offset.

### 3. PC: start the receiver

```powershell
.\experiment\receiver\build\ucv-receiver.exe `
    --phone 192.168.0.101 `
    --run-id <run_id from the phone> `
    --duration 60 --warmup 5 `
    --out experiment\results
```

Expect `[clock] offset = ... best RTT = ...`, then `[recv] listening...`.

> A mismatched `run_id` means every frame is discarded as belonging to another
> run. That is the protection working, not a fault — but it looks like total
> packet loss, so check this first if nothing arrives.

### 4. Phone: start streaming

**2. Start Raw UDP**

---

## What healthy looks like

On the phone panel:

```
c2.mtk.avc.encoder 1280x720@30 4000000bps keyint=1s hw=yes
cap=1234 dec=1234 enc=1233 sent=1233
decerr=0 drop=0 senderr=0
decode=3ms encode=6ms payload=2048KB
```

- `hw=yes` — a software encoder invalidates the run (harness spec §2)
- `cap ≈ dec ≈ enc ≈ sent`, all climbing at ~30/s
- `decerr` / `drop` / `senderr` at or near zero
- `encode` well under 33 ms (the frame interval at 30 fps)

On the PC, a line every 5 s with a climbing frame count.

---

## When it does not work

| Symptom | Cause |
|---|---|
| Receiver: `clock sync failed` | Control channel not started, or wrong phone IP |
| Receiver: 0 frames, no errors | `run_id` mismatch, or wrong subnet — re-run `Test-Connectivity.ps1` |
| Phone: `encoder init failed` | No hardware H.264 encoder available |
| Phone: `hw=NO` | Software encoder — run is invalid, do not record it |
| Phone: `cap` rises, `dec` flat | Every frame failing to decode — usually a resolution mismatch |
| Phone: `enc` rises, `sent` flat | Transport not connected — check the PC IP entered on the phone |
| Phone: `drop` climbing | Encoder falling behind — lower resolution/fps before blaming a protocol |
| Colours wrong | Chroma order — re-run `ucv-nv12test` |

---

## After the run

```powershell
python experiment\analysis\analyze.py experiment\results\*.ndjson --out summary.md
```

> **The first run on a given phone is bring-up, not measurement.** Do not
> record its numbers. Confirm the pipeline is healthy first, then start the
> real matrix (`docs/04-run-procedure.md` §4).
