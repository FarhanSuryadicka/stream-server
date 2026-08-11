# Panduan Lengkap — UCV Transport Lab

Panduan dari nol: menyiapkan PC, membangun aplikasi HP, dan menjalankan
pengukuran lewat **dashboard web** maupun **aplikasi desktop**.

Terakhir diverifikasi: 12 Agustus 2026.

> Dokumen ini menggantikan kebutuhan membaca banyak file sekaligus. Untuk detail
> tertentu: `experiment/RUNBOOK-CURRENT.md` (operasional rig),
> `docs/HANDOFF-CLAUDE.md` (status protokol), `server/README.md` (desain
> aplikasi desktop).

---

## Daftar isi

1. [Gambaran sistem](#1-gambaran-sistem)
2. [Prasyarat PC](#2-prasyarat-pc)
3. [Build receiver PC](#3-build-receiver-pc)
4. [Build aplikasi HP](#4-build-aplikasi-hp)
5. [Build aplikasi desktop](#5-build-aplikasi-desktop)
6. [Menyiapkan rig](#6-menyiapkan-rig)
7. [Menjalankan pengukuran — dashboard web](#7-menjalankan-pengukuran--dashboard-web)
8. [Menjalankan pengukuran — aplikasi desktop](#8-menjalankan-pengukuran--aplikasi-desktop)
9. [Membaca hasil](#9-membaca-hasil)
10. [Troubleshooting](#10-troubleshooting)
11. [Referensi cepat](#11-referensi-cepat)

---

## 1. Gambaran sistem

Kamera fisheye USB menempel ke HP Android. HP melakukan capture, decode, dan
encode H.264, lalu mengirim ke PC dengan protokol yang sedang diuji. PC
mengukur dan mencatat.

```text
Kamera UVC (MJPEG)
      |
      v
HP Android
  - capture USB
  - decode JPEG -> NV12
  - encode H.264 (MediaCodec, hardware)
  - kirim lewat protokol terpilih
      |
      |  video  ===============>  PC receiver  -> NDJSON + metrik
      |  <=========== control ==  clock sync, START/STOP, RTT
      v
PC (ucv-receiver.exe)
      |
      +--> dashboard web     (http://127.0.0.1:8088)
      +--> aplikasi desktop  (ucv-monitor.exe)
```

**Dua dashboard, satu sumber data.** Keduanya membaca NDJSON yang sama dan
menjalankan `ucv-receiver.exe` yang sama. Pilih salah satu, atau pakai
bergantian — tidak saling memengaruhi. Angkanya sudah diverifikasi identik.

### Delapan protokol

| Protokol | Nama di UI | Catatan |
|---|---|---|
| `raw_udp` | Raw UDP (H.264) | baseline, sudah diuji HP -> PC |
| `rtp_udp` | RTP/UDP (H.264) | RFC 6184, header ekstensi RFC 8285 |
| `rtsp` | RTSP (H.264, signalled) | OPTIONS/DESCRIBE/SETUP/PLAY, paket sama dengan rtp_udp |
| `srt` | SRT (H.264, 20 ms) | latency 20 ms, **enkripsi off** |
| `mjpeg` | MJPEG over HTTP | membawa JPEG, **bukan** H.264 |
| `hls` | HLS (H.264, 1 s segments) | kontras latensi tinggi |
| `rtmp` | RTMP (H.264, plaintext) | **plaintext, bukan RTMPS** |
| `webrtc` | WebRTC (H.264, DTLS-SRTP) | butuh signalling, LAN saja |

> **Penting untuk laporan:** MJPEG membawa JPEG tanpa H.264, jadi angka
> bandwidth-nya tidak sebanding dengan tujuh protokol lain. Latensi dan control
> RTT-nya tetap sebanding.

---

## 2. Prasyarat PC

Semua **native Windows**. Tidak perlu WSL, tidak perlu MSYS2.

| Kebutuhan | Untuk apa | Cara cek |
|---|---|---|
| Windows PowerShell | menjalankan semua script | sudah ada |
| CMake 3.21+ | build receiver dan desktop | `cmake --version` |
| MinGW-w64 | compiler C/C++ | `g++ --version` |
| JDK 17 + Android SDK | build APK | `java -version` |
| FFmpeg | preview kamera | `ffmpeg -version` |
| Qt 6.5+ | **hanya** untuk aplikasi desktop | `dir C:\Qt` |

### Kalau PowerShell menolak menjalankan .ps1

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script.ps1>
```

### Kalau karakter terminal rusak (`ΓÇö`)

```powershell
chcp 65001
```

Itu masalah code page terminal saja; data NDJSON tetap UTF-8 dan benar.

### FFmpeg

Preview memerlukan `ffmpeg.exe` di `PATH`, atau di:

```text
C:\Tools\ffmpeg\bin\ffmpeg.exe
```

Tanpa FFmpeg, pengukuran tetap berjalan normal — hanya gambarnya tidak muncul.
Kecuali protokol `mjpeg`, yang preview-nya tidak butuh FFmpeg sama sekali.

### Qt (opsional, hanya untuk aplikasi desktop)

```powershell
pip install aqtinstall
python -m aqt install-qt   windows desktop 6.8.1 win64_mingw -O C:/Qt
python -m aqt install-tool windows desktop tools_mingw1310    -O C:/Qt
```

Unduhan sekitar 1,5 GB. Kalau hanya mau pakai dashboard web, lewati langkah ini.

---

## 3. Build receiver PC

**Ini wajib.** Kedua dashboard menjalankan binary yang sama.

```powershell
cd C:\Users\user\stream-server
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\build-receiver.ps1
```

Build dianggap sehat bila muncul:

```text
100% tests passed, 0 tests failed out of 3
```

Hasilnya:

```text
experiment\receiver\build\ucv-receiver.exe
```

Gunakan `-Clean` setelah mengubah toolchain atau header wire:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\build-receiver.ps1 -Clean
```

### Kenapa tiga test

| Test | Yang dijaga |
|---|---|
| `primitives` | layout wire, CRC, offset clock, percentile, akuntansi loss/reorder |
| `nv12` | konversi JPEG -> NV12: geometri plane, urutan chroma U-sebelum-V |
| `jpeg_reuse` | jalur decoder planar, reuse buffer, penolakan input cacat |

Test `nv12` pernah menangkap bug NV21-vs-NV12 yang tidak crash — hanya membuat
warna tertukar, dan akan disalahkan ke kamera. Jalankan sebelum memercayai
angka apa pun.

---

## 4. Build aplikasi HP

### 4.1 Wireless debugging (disarankan)

Port USB HP dipakai kamera, jadi ADB lewat kabel tidak bisa saat streaming.

Di HP: **Developer options -> Wireless debugging**.

Pertama kali (perlu pairing):

```powershell
adb pair <IP-HP>:<PAIRING-PORT>
```

Masukkan kode 6 digit yang ditampilkan HP. Lalu sambungkan:

```powershell
adb connect 192.168.137.139:<DEBUG-PORT>
adb devices
```

> Port wireless debugging **berubah** setelah HP reboot atau fitur dimatikan.
> Selalu baca port yang sedang ditampilkan HP.

### 4.2 Build APK

Setelah perubahan Kotlin saja:

```powershell
.\gradlew.bat :app:assembleDebug
```

Setelah perubahan C/C++ atau JNI — **selalu clean build**:

```powershell
.\gradlew.bat :app:clean :app:assembleDebug
```

> CMake incremental pernah menyajikan `.so` lama meski task dilaporkan
> dijalankan. Kalau JNI melempar `UnsatisfiedLinkError` untuk simbol yang baru
> ditambahkan, itu penyebabnya — bukan kode C-nya.

Kalau Gradle memakai konfigurasi lama (pernah terjadi, menunjuk ke repo lama):

```powershell
.\gradlew.bat :app:assembleDebug --no-configuration-cache --rerun-tasks
```

APK ada di:

```text
app\build\outputs\apk\debug\app-debug.apk
```

### 4.3 Install

```powershell
adb -s 192.168.137.139:<DEBUG-PORT> install -r .\app\build\outputs\apk\debug\app-debug.apk
adb -s 192.168.137.139:<DEBUG-PORT> shell am start -n com.anjas.uvcserver/.MainActivity
```

Alternatif bila HP tersambung USB ke PC (kamera dilepas dulu):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\install-phone.ps1 -Clean
```

Setelah itu cabut HP dari PC dan pasang kamera lewat OTG.

### 4.4 Verifikasi simbol native (opsional)

Setelah perubahan besar di C/C++:

```powershell
$nm = "C:\Users\user\AppData\Local\Android\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe"
$so = "app\build\intermediates\merged_native_libs\debug\mergeDebugNativeLibs\out\lib\arm64-v8a\libuvcserver.so"
& $nm -D --defined-only $so | Select-String "ucv_transport_"
```

Semua transport harus `T` (implementasi nyata). Kalau ada yang `W`, itu stub
NULL dan protokolnya akan menolak jalan.

---

## 5. Build aplikasi desktop

**Opsional.** Lewati kalau hanya mau pakai dashboard web.

```powershell
.\server\build.ps1
```

Itu saja. Script akan configure, build, dan menjalankan test.

| Perintah | Kegunaan |
|---|---|
| `.\server\build.ps1` | build penuh + test |
| `.\server\build.ps1 -CoreOnly` | logika pengukuran + test saja, **tanpa Qt** |
| `.\server\build.ps1 -Clean` | hapus folder build dulu |
| `.\server\build.ps1 -Run` | langsung jalankan setelah sukses |

### Dua jebakan yang sudah ditangani script

**1. Toolchain harus cocok dengan Qt.** Qt 6.8.1 dibangun dengan GCC 13.1.0.
Memakai GCC lain akan **link sukses**, tetapi aplikasi gagal saat *start*:

```text
The procedure entry point _ZNKSt25__codecvt_utf8_utf16... could not be located
```

Tidak ada petunjuk apa pun di log build. Script memaksa compiler yang benar.

**2. CMake menyimpan compiler di cache.** Setelah ganti toolchain atau versi Qt,
konfigurasi ulang di tempat tetap memakai compiler lama. Gunakan `-Clean`.

---

## 6. Menyiapkan rig

### Konfigurasi jaringan

| Perangkat | Alamat |
|---|---|
| PC Ethernet / receiver | `192.168.137.1` |
| HP | `192.168.137.139` |
| Gateway HP | `192.168.137.1` |
| Router (manajemen) | `192.168.137.223` |

PC berbagi internet dari Wi-Fi ke Ethernet lewat Windows Internet Connection
Sharing. PC tersambung kabel LAN ke router; HP tersambung Wi-Fi ke router yang
sama.

> **Jangan** memasukkan IP Wi-Fi PC (`192.168.10.x`) sebagai alamat receiver.
> Jalur HP memakai `192.168.137.1`. Salah di sini terlihat persis seperti
> kegagalan protokol: receiver menunggu dan tidak ada frame yang datang.

Cek cepat:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\Get-RigInfo.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\Test-Connectivity.ps1
```

### Port

| Port | Untuk |
|---|---|
| 8200/UDP | control (clock sync, START/STOP, GET_MODE) |
| 8201/UDP | Raw UDP video |
| 5004/UDP | RTP video |
| 8202/UDP | SRT |
| 8181/TCP | MJPEG HTTP |
| 8204/TCP | HLS HTTP |
| 8554/TCP | RTSP signalling |
| 1935/TCP | RTMP |
| 8203/TCP | WebRTC signalling |
| 8088/TCP | dashboard web (lokal) |

Windows Firewall harus mengizinkan port protokol yang dipakai pada jaringan rig.

### Menyiapkan HP

1. Pasang kamera ke HP lewat OTG
2. Buka aplikasi, pilih menu **Website / Auto**
3. Tekan **Open USB Camera**
4. Izinkan CAMERA dan RECORD_AUDIO bila diminta
5. Izinkan akses USB bila dialog muncul
6. Tunggu sampai muncul:

```text
Camera agent ready on 192.168.137.139:8200 - choose mode on PC
Control: READY  192.168.137.139:8200
Pipeline: waiting for website
```

> HyperOS/MIUI meminta izin CAMERA/RECORD_AUDIO walaupun aplikasi ini hanya
> memakai raw USB host. Izin tetap harus diberikan.

Biarkan layar HP menyala. Untuk perbandingan yang serius, jaga posisi kamera,
posisi router, dan kondisi daya tetap sama antar-run.

---

## 7. Menjalankan pengukuran — dashboard web

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\run-dashboard.ps1
```

Browser membuka `http://127.0.0.1:8088/`.

### Langkah

1. Pastikan **Phone IP** = `192.168.137.139`
2. Tekan **Refresh camera modes** — daftar diambil dari descriptor MJPEG kamera
3. Pilih **MJPEG resolution** (FPS dipilih otomatis, tertinggi yang kamera
   iklankan untuk resolusi itu)
4. Pilih **Protocol**
5. Isi **Duration** dan **Warmup** (awal: 60 dan 5)
6. Biarkan **Run ID** kosong — dashboard membuatnya otomatis
7. Tekan **Start measurement** satu kali

Urutan otomatisnya:

```text
clock sync PC <-> HP
-> receiver bind port video
-> run hash dikirim ke HP
-> START dikirim ke HP
-> HP: buka mode MJPEG -> decode -> encode H.264 -> kirim
-> PC menerima, mengukur, mencatat, menampilkan preview
-> setelah durasi: STOP dikirim ke HP
```

Console yang sehat:

```text
[clock] offset = ... ms, best RTT = ... ms
[phone] pipeline started remotely: 1280x720@30 -> this PC:8201
[recv] listening...
[recv]    101 frames  transport p50=... ms  jitter=... ms  gaps=0
```

Di HP statusnya berubah jadi `Pipeline: STREAMING`.

> **Gunakan tombol Stop**, jangan menutup paksa terminal receiver. Shutdown
> normal menulis baris `summary` dan menghentikan pipeline HP dengan rapi. Run
> tanpa `summary` akan selamanya berstatus `RUNNING` dan tidak pernah dihitung
> valid.

### Dashboard tidak hot reload

Setelah mengubah kode HTML/Python:

1. `Ctrl+C` di terminal dashboard
2. jalankan lagi script-nya
3. `Ctrl+F5` di browser (hard reload)

---

## 8. Menjalankan pengukuran — aplikasi desktop

```powershell
.\server\run.ps1
```

Atau double-click `server\build\ui\ucv-monitor.exe`.

Langkahnya sama persis dengan dashboard web: Refresh camera modes, pilih
resolusi, pilih protokol, Start measurement.

| Perintah | Kegunaan |
|---|---|
| `.\server\run.ps1` | jalankan normal |
| `.\server\run.ps1 -Console` | tampilkan log Qt/QML, untuk saat bermasalah |

### Perbedaan dengan dashboard web

| Hal | Web | Desktop |
|---|---|---|
| Angka metrik | sama | **sama persis** (terverifikasi 14 run) |
| Comparison group | `.dashboard-labels.json` | file yang sama |
| Preview | `<img>` MJPEG | Qt image provider |
| Butuh browser | ya | tidak |
| Butuh Qt | tidak | ya |

Keduanya bisa membaca run yang sama. Yang **tidak** boleh: menjalankan
pengukuran dari keduanya sekaligus — dua receiver akan berebut port yang sama.

---

## 9. Membaca hasil

### File

```text
experiment\results\receiver-<run_id>.ndjson
```

Log menyimpan **timestamp mentah saja**, tidak pernah nilai turunan. Jadi kalau
rumus metrik diperbaiki, semua run lama bisa dihitung ulang tanpa mengukur
ulang.

### Kapan sebuah run valid

- `frames received > 0`
- `clock_status = OK`
- `wrong-run frames = 0`
- decode/send error nol atau bisa dijelaskan
- hardware encoder `hw=yes`
- tidak ada kegagalan reassembly signifikan

Run pertama setelah install atau reconnect dianggap **bring-up**, bukan hasil
resmi. Pakai run berikutnya.

### Arti metrik

**Transport latency** — `waktu terima PC - waktu kirim HP`, dikoreksi clock
offset. Ini metrik pemeringkat utama.

**Glass-to-glass** — termasuk capture dan encode. Yang dirasakan manusia, tapi
pembeda protokol yang buruk karena biaya encode yang konstan menekan
perbedaannya.

**Jitter** — estimator RFC 3550. Tidak bergantung pada offset clock absolut,
jadi ini yang paling bisa dipercaya di antara metrik satu arah.

**Loss** — artinya **berbeda per protokol**:

| Protokol | Basis |
|---|---|
| raw_udp | UDP packet sequence yang hilang |
| rtp_udp, rtsp | RTP sequence yang hilang |
| srt | paket yang libsrt deteksi hilang; recovered dicatat terpisah |
| mjpeg | TCP retransmission dari `TCP_INFO` |
| hls, rtmp, webrtc | gap frame sequence |

Dashboard menampilkan basisnya di sebelah angka. **Jangan merata-ratakan grup
yang basisnya campur** — kedua dashboard menolak melakukannya dan mengatakan
alasannya.

**Overhead** — `n/a` untuk WebRTC. Framing RTP/SRTP ada di bawah API, jadi tidak
terobservasi. Butuh packet capture.

### Batasan yang harus disebut di laporan

- **Latensi satu arah punya error residual ±1–3 ms.** Perbedaan di bawah ~5 ms
  antar-protokol **tidak resolvable** oleh harness ini.
- **Loss dari gap sequence adalah batas bawah.** Kehilangan di ujung run tidak
  meninggalkan gap.
- **Overhead aplikasi bukan overhead wire.** Retransmisi dan biaya header perlu
  packet capture.
- **MJPEG membawa JPEG, bukan H.264.** Bandwidth-nya tidak sebanding.
- **WebRTC diukur tanpa STUN/TURN** — ini LAN best case, bukan hasil NAT
  traversal.

### Analisis manual

```powershell
python .\experiment\analysis\analyze.py .\experiment\results\*.ndjson --out .\experiment\results\summary.md
```

---

## 10. Troubleshooting

### `Refresh camera modes` gagal

Penyebab umum: belum menekan Open USB Camera, izin USB belum diberikan, IP HP
berubah, atau UDP 8200 diblokir firewall.

Tindakan: di HP tekan **Close** lalu **Open USB Camera**, tunggu
`Control: READY`, pastikan Phone IP benar, lalu coba lagi.

### `clock sync failed`

Control agent HP tidak menjawab. Periksa IP HP, status `Control: READY`,
subnet, dan firewall UDP 8200. Receiver **sengaja** menolak jalan tanpa clock
sync — tanpa itu, angka latensi hanyalah selisih clock.

### `phone rejected remote START`

Lihat log HP. Penyebab yang pernah muncul: mode terlalu berat, hardware encoder
menolak resolusi, stream USB gagal dimulai, atau pipeline sebelumnya belum
berhenti. Coba resolusi lebih rendah; kalau masih, Close lalu Open USB Camera.

### `SUBMITURB failed errno=12 (Out of memory)`

Dari alokasi transfer USB isochronous Android. Stop run, Close kamera,
cabut/pasang ulang kamera bila perlu, Open lagi, pilih resolusi lebih rendah.

### Pipeline jalan tetapi `cap=0`

UVC bernegosiasi tetapi tidak menghasilkan frame — biasanya mode terlalu berat
atau transfer USB stall. Pilih resolusi lebih rendah.

### Receiver menerima 0 frame dan `wrong-run` besar

Run hash HP dan PC berbeda. Biasanya karena dashboard/receiver lama masih
berjalan. Restart dashboard, jangan mulai pipeline manual dari HP, mulai run
baru.

### Preview hitam

- `frames received = 0` -> masalah sebelum decoder preview
- status `waiting` -> H.264 belum diterima
- status `error` -> periksa FFmpeg
- frame bertambah tapi gambar hitam -> periksa output kamera

### Aplikasi desktop: "procedure entry point ... could not be located"

Toolchain tidak cocok dengan Qt. Build ulang bersih:

```powershell
.\server\build.ps1 -Clean
```

### Aplikasi desktop tidak menampilkan apa-apa

Jalankan dengan console untuk melihat error QML:

```powershell
.\server\run.ps1 -Console
```

### JNI `UnsatisfiedLinkError` untuk simbol baru

CMake incremental menyajikan `.so` lama. Clean build:

```powershell
.\gradlew.bat :app:clean :app:assembleDebug
```

### Wireless ADB hilang

Pastikan Wireless debugging aktif, baca port baru di HP, lalu:

```powershell
adb connect 192.168.137.139:<PORT-BARU>
adb devices
```

---

## 11. Referensi cepat

### Urutan dari nol

```powershell
cd C:\Users\user\stream-server

# 1. Receiver (wajib untuk keduanya)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\build-receiver.ps1

# 2. APK
.\gradlew.bat :app:clean :app:assembleDebug
adb connect 192.168.137.139:<PORT>
adb -s 192.168.137.139:<PORT> install -r .\app\build\outputs\apk\debug\app-debug.apk

# 3. Desktop (opsional)
.\server\build.ps1
```

### Menjalankan

```powershell
# Web
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experiment\run-dashboard.ps1

# Desktop
.\server\run.ps1
```

### Validasi cepat sebelum ambil data resmi

Jalankan satu run `320x240` selama 30–60 detik dan pastikan:

```text
preview        = live
frames         terus bertambah
gaps           = 0 atau sangat kecil
wrong-run      = 0
clock_status   = OK
hw             = yes
cap ~= dec ~= enc ~= sent
```

### Urutan resolusi yang disarankan

```text
320x240 -> 640x480 -> 1280x720 -> 1920x1080 -> lebih tinggi
```

Resolusi tinggi yang diiklankan kamera belum tentu stabil melalui USB
isochronous, decoder, atau hardware encoder HP. Descriptor berarti kamera
mendukung mode itu, bukan jaminan seluruh pipeline sanggup.

Untuk perbandingan: minimal **tiga run valid** per resolusi per protokol,
dengan router, posisi, durasi, warmup, dan trafik jaringan yang sama.

### Semua script

| Script | Kegunaan |
|---|---|
| `experiment\build-receiver.ps1` | build receiver PC + test |
| `experiment\run-dashboard.ps1` | jalankan dashboard web |
| `experiment\install-phone.ps1` | build + install APK lewat USB |
| `experiment\Get-RigInfo.ps1` | IP PC mana yang dipakai |
| `experiment\Test-Connectivity.ps1` | pre-flight: subnet, reachability, firewall |
| `experiment\Measure-Baseline.ps1` | baseline jaringan |
| `server\build.ps1` | build aplikasi desktop |
| `server\run.ps1` | jalankan aplikasi desktop |

### Status protokol saat ini

Tujuh dari delapan protokol punya sender dan receiver lengkap dan sudah
menghasilkan angka di loopback PC. Yang **belum pernah diuji dengan HP dan
kamera nyata**: RTSP, SRT, HLS, RTMP, WebRTC.

Anggap run pertama tiap protokol sebagai bring-up, bukan pengukuran.
