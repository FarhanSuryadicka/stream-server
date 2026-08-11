# Handoff Claude Code — UVC Transport Lab

Tanggal handoff: 11 Agustus 2026

## 1. Lokasi proyek yang benar

Repository aktif:

```text
C:\Users\user\stream-server
```

Jangan melanjutkan perubahan di repository lama:

```text
C:\Users\user\ucvserver
```

Semua perintah dalam dokumen ini dijalankan dari `C:\Users\user\stream-server`.

## 2. Tujuan proyek

Aplikasi Android menerima MJPEG dari kamera USB UVC, lalu mengirim video ke PC
melalui beberapa protokol untuk dibandingkan. PC receiver dan dashboard
mengukur serta menampilkan:

- network/transport latency;
- jitter;
- packet loss atau retransmission transport;
- frame gap dan reliability;
- goodput;
- control-channel RTT;
- live camera preview;
- run history dan perbandingan resolusi.

Kotlin/Compose adalah UI dan USB-permission glue. Capture, decode, H.264 encode,
transport, serta instrumentation utama berada di native C/C++.

## 3. Konfigurasi jaringan terakhir

```text
PC/Ethernet/receiver : 192.168.137.1
HP                   : 192.168.137.139
Gateway HP           : 192.168.137.1
Router management    : 192.168.137.223
Control UDP          : 8200
Raw UDP video        : 8201
SRT                   : 8202
MJPEG HTTP           : 8181
RTP/UDP               : 5004
```

Windows Internet Connection Sharing membagikan internet Wi-Fi PC ke Ethernet.
Alamat dapat berubah; verifikasi dengan `ipconfig` dan Wi-Fi details HP sebelum
testing.

Wireless ADB terakhir menggunakan HP `192.168.137.139`. Port wireless-debug
Android bersifat dinamis, jadi lihat kembali menu **Proses debug nirkabel**.

## 4. Status protokol

| Protokol | Android | PC/dashboard | Status sebenarnya |
|---|---|---|---|
| Raw UDP H.264 | selesai | selesai | Sudah diuji HP → PC |
| MJPEG/HTTP | selesai | selesai | Sudah diuji, preview web bekerja |
| RTP/UDP H.264 | selesai | selesai | Data plane selesai; RTSP signalling belum |
| SRT H.264 | selesai dan build lulus | selesai dan build lulus | Belum diuji pada HP/kamera |
| WebRTC | stub | belum | Belum diimplementasikan |
| RTMPS | stub | belum | Belum diimplementasikan |
| HLS/DASH | stub | belum | Belum diimplementasikan |

Jangan menyebut RTP/UDP saat ini sebagai RTSP penuh. Belum ada RTSP
`DESCRIBE`, `SETUP`, `PLAY`, atau SDP signalling.

## 5. Pipeline Android

Pipeline H.264:

```text
UVC MJPEG capture
  -> libjpeg-turbo decode
  -> NV12
  -> hardware MediaCodec H.264
  -> transport terpilih
```

Pipeline MJPEG/HTTP meneruskan JPEG kamera tanpa H.264 re-encode.

UI HP memiliki mode sederhana sebagai camera agent yang dikendalikan website,
serta mode manual untuk diagnosis. Pada alur normal, operator cukup membuka
kamera USB; website memilih resolusi MJPEG yang benar-benar dilaporkan kamera,
FPS terbaik dipilih otomatis, lalu website menjalankan control dan pipeline.

## 6. Protokol yang sudah ada

### Raw UDP

- Header instrumentation 56 byte pada setiap fragmen.
- Payload fragmen 1200 byte.
- `packet_seq` dipakai untuk packet loss real-time.
- Receiver melakukan reassembly dan mencatat packet loss, reorder, duplicate,
  dan frame gap.

### RTP/UDP

- RFC 6184 single NAL dan FU-A.
- Header instrumentation dibawa melalui RTP header extension RFC 8285.
- Receiver menggunakan RTP packet sequence untuk loss/reorder/duplicate.
- Preview H.264 bekerja melalui FFmpeg.
- Belum ada RTSP signalling.

### MJPEG/HTTP

- JPEG kamera diteruskan melalui multipart HTTP.
- Preview browser tidak memerlukan H.264 decode.
- Packet-loss equivalent memakai TCP retransmission dari `TCP_INFO`, bukan
  sequence gap palsu.
- Jika `TCP_INFO` tidak tersedia, UI harus menyebut fallback sebagai frame
  loss, bukan TCP packet loss.

### SRT — perubahan terbaru

- Official Haivision SRT v1.5.6 divendor sebagai source biasa di:

  ```text
  app/src/main/cpp/srt
  ```

- Tidak ada nested `.git`; file terbesar sekitar 514 KB sehingga aman untuk
  GitHub normal.
- Metadata upstream ada di `app/src/main/cpp/srt/VENDORED-UCV.md`.
- Android adalah SRT caller; PC adalah listener pada port 8202.
- Live/message mode.
- `SRTO_LATENCY=20 ms` secara eksplisit.
- `SRTO_TLPKTDROP=1`.
- Payload SRT 1256 byte: header UCV 56 byte + fragmen H.264 maksimum 1200 byte.
- Encryption sengaja `off` untuk profil awal dan harus dilabeli demikian.
- Dashboard memiliki pilihan `SRT (H.264, 20 ms)` dan live preview.
- NDJSON menyimpan:

  ```text
  packets_received
  packets_lost
  packets_recovered
  ```

- Untuk SRT, `packets_lost` berarti paket yang terdeteksi missing oleh libsrt.
  `packets_recovered` adalah retransmitted packets yang berhasil diterima.
  Detected loss bukan otomatis final media/frame loss.

File penting SRT:

```text
app/src/main/cpp/ucv_transport_srt.c
app/src/main/cpp/CMakeLists.txt
app/src/main/cpp/jni_bridge.c
experiment/receiver/src/main.cpp
experiment/receiver/src/ucv_log.cpp
experiment/receiver/include/ucv_log.h
experiment/receiver/CMakeLists.txt
experiment/dashboard/server.py
experiment/dashboard/index.html
```

## 7. Status build dan smoke test terakhir

Receiver PC:

```powershell
cmake -S experiment/receiver -B experiment/receiver/build
cmake --build experiment/receiver/build --config Release -j 12
ctest --test-dir experiment/receiver/build -C Release --output-on-failure
```

Hasil terakhir:

```text
ucv-receiver build: SUCCESS
native tests       : 2/2 PASSED
```

Android:

```powershell
.\gradlew.bat :app:assembleDebug :app:testDebugUnitTest
```

Hasil terakhir:

```text
BUILD SUCCESSFUL
APK debug dibuat
unit test lulus
APK TIDAK di-install ke HP
```

APK berada di:

```text
app\build\outputs\apk\debug\app-debug.apk
```

Simbol berikut sudah diverifikasi ada di `libuvcserver.so`:

```text
ucv_transport_srt
srt_connect
srt_startup
```

Dashboard:

- `python -m py_compile experiment/dashboard/server.py` lulus.
- Smoke test synthetic SRT lulus:
  - 180 received;
  - 20 detected lost;
  - dashboard menghasilkan 10% SRT detected loss;
  - 16 recovered tersimpan terpisah.

Belum ada physical end-to-end SRT test karena HP sedang digunakan. Jangan
mengklaim SRT sudah tervalidasi pada perangkat.

## 8. Working tree yang belum di-commit

Pada saat handoff, perubahan SRT masih berada di working tree. Jalankan:

```powershell
git status --short
git diff --check
```

Perubahan yang diharapkan meliputi:

```text
app/src/main/cpp/CMakeLists.txt
app/src/main/cpp/jni_bridge.c
app/src/main/cpp/srt/                  (baru)
app/src/main/cpp/ucv_transport_srt.c  (baru)
experiment/README.md
experiment/RUNBOOK-CURRENT.md
experiment/dashboard/index.html
experiment/dashboard/server.py
experiment/docs/02-wire-format.md
experiment/docs/03-implementation-plan.md
experiment/receiver/CMakeLists.txt
experiment/receiver/include/ucv_log.h
experiment/receiver/src/main.cpp
experiment/receiver/src/ucv_log.cpp
```

Jangan menghapus atau mengganti perubahan tersebut. Review lalu commit jika
diminta user. Tidak ada nested `.git` di source SRT.

## 9. Cara menjalankan dashboard

Build receiver terlebih dahulu bila perlu, kemudian:

```powershell
cd C:\Users\user\stream-server
powershell -ExecutionPolicy Bypass -File .\experiment\run-dashboard.ps1
```

Buka:

```text
http://127.0.0.1:8088
```

Alur test normal:

1. Kamera USB terpasang ke HP.
2. Buka aplikasi dan tekan **Open USB Camera**.
3. Pastikan log mengatakan perangkat UVC terbuka.
4. Di website isi Phone IP `192.168.137.139` atau alamat terbaru.
5. Tekan **Refresh camera modes**.
6. Pilih resolusi MJPEG yang dilaporkan kamera. FPS dipilih otomatis.
7. Pilih protokol.
8. Isi duration dan warmup.
9. Tekan **Start measurement**.
10. Tunggu run selesai atau tekan Stop.
11. Periksa preview, live charts, NDJSON, dan Run History.

Untuk test SRT nanti, pasang APK terbaru melalui wireless ADB hanya ketika HP
sudah tersedia. Jangan melakukan instalasi tanpa izin user.

## 10. Interpretasi metrik loss

Jangan memakai satu definisi loss secara buta untuk seluruh protokol:

| Protokol | Basis metrik utama |
|---|---|
| Raw UDP | missing application UDP `packet_seq` |
| RTP/UDP | missing RTP/application packet sequence |
| MJPEG/TCP | TCP retransmission rate dari `TCP_INFO` |
| SRT | libsrt detected missing packets; recovered dicatat terpisah |

Frame gap tetap dicatat untuk semuanya sebagai kegagalan media yang terlihat
receiver. Pada reliable transport, network packet loss dapat naik sementara
frame gap tetap nol karena retransmission berhasil.

## 11. Masalah yang pernah diperbaiki

- Pipeline awal berjalan tetapi `cap=0`, karena UVC transfer tidak benar-benar
  menghasilkan frame.
- Mode USB tertentu gagal dengan `ENOMEM`/zero transfers; mode rendah
  `320x240@20` berhasil.
- First-frame markers sudah ditambahkan:
  `FIRST FRAME CAPTURED`, `FIRST FRAME DECODED`, `FIRST FRAME SENT`.
- Dashboard sekarang live membaca NDJSON saat receiver berjalan.
- Run ID dibuat unik agar run resolusi kedua tidak menimpa log pertama.
- Raw UDP/RTP memakai packet sequence sehingga loss fragmen terlihat real-time.
- MJPEG memakai TCP retransmission statistics.
- `libusb-cmake` sebelumnya merupakan gitlink/nested repository bermasalah;
  sekarang sudah menjadi vendored source biasa. Jangan mengembalikannya menjadi
  submodule tanpa alasan kuat.

## 12. Langkah lanjutan yang disarankan

Prioritas terdekat:

1. Review `git diff` perubahan SRT.
2. Saat HP tersedia, install APK melalui wireless ADB dengan izin user.
3. Jalankan SRT pada `320x240` lebih dahulu.
4. Verifikasi log HP menunjukkan SRT connected dan first-frame markers.
5. Verifikasi receiver menerima frame dan dashboard menampilkan SRT detected
   loss/recovered secara real-time.
6. Ulangi minimal tiga run per resolusi sebelum membandingkan rata-rata.
7. Setelah SRT valid, pilih protokol berikutnya.

Pilihan implementasi berikutnya:

- menyelesaikan RTSP signalling di atas RTP yang sudah ada; atau
- WebRTC sebagai protokol baru yang paling relevan untuk low-latency browser.

Menyelesaikan RTSP signalling lebih kecil risikonya karena RTP data plane,
receiver, preview, dan metriknya sudah tersedia.

## 13. Batasan dan aturan penting

- Jangan install APK saat HP sedang digunakan tanpa izin user.
- Jangan mengubah vendored dependency secara sembarang.
- Hanya MJPEG camera modes yang ditampilkan; jangan campurkan mode YUV.
- FPS pada website dipilih otomatis dari mode terbaik kamera untuk resolusi
  yang dipilih.
- Jangan fallback diam-diam ke Raw UDP ketika protokol belum ada.
- Jangan menamai RTP/UDP sebagai RTSP sebelum signalling selesai.
- SRT saat ini tidak terenkripsi.
- Perbandingan one-way latency di bawah sekitar 5 ms tidak resolvable secara
  kuat karena residual clock-sync error.
- Pertahankan `AGENTS.md` dan `CLAUDE.md` sinkron jika salah satunya diubah.

