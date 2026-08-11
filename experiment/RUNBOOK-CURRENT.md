# UCV Transport Lab — Runbook Terbaru

Dokumen ini adalah panduan operasional utama untuk build saat ini. Gunakan
dokumen ini ketika memindahkan pekerjaan ke Claude atau sesi pengembangan lain.

> `experiment/CHECKLIST.md` dan `experiment/docs/04-run-procedure.md` masih
> menjelaskan alur manual lama dan contoh subnet `192.168.0.x`. Untuk alur
> website-controlled terbaru, dokumen ini yang berlaku.

## 1. Tujuan dan arsitektur

Aplikasi Android menerima video dari kamera USB UVC. Kamera hanya dienumerasi
dari descriptor **MJPEG** (`UVC_VS_FORMAT_MJPEG`); mode YUYV/YUV tidak dimasukkan
ke daftar website.

Saat pengukuran berjalan:

```text
USB UVC camera (MJPEG)
  -> Android JPEG decode
  -> Android hardware H.264 encode
  -> Raw UDP melalui Wi-Fi
  -> PC receiver pada UDP 8201
  -> log NDJSON + metrik + preview website
```

Control channel PC -> HP memakai UDP 8200 untuk:

- sinkronisasi clock;
- discovery resolusi/FPS MJPEG kamera;
- mengirim run hash;
- remote Start/Stop pipeline;
- mengukur control RTT selama video berjalan.

Kamera hanya menjalankan **satu resolusi pada satu waktu**. Semua komponen HP
siap menerima perintah, tetapi website menghentikan mode lama sebelum memulai
mode berikutnya agar stream USB tidak saling berebut bandwidth.

## 2. Konfigurasi rig saat ini

| Perangkat/interface | Alamat |
|---|---:|
| PC Ethernet / receiver / gateway ICS | `192.168.137.1` |
| Router/AP TP-Link TL-WR840N | `192.168.137.223` |
| HP POCO X7 | `192.168.137.139` |
| Gateway HP | `192.168.137.1` |
| PC Wi-Fi internet | `192.168.10.79` (dapat berubah) |
| Control HP | UDP `8200` |
| Video receiver PC | UDP `8201` |
| Dashboard lokal | `http://127.0.0.1:8088/` |

PC membagikan internet dari Wi-Fi ke Ethernet menggunakan Windows Internet
Connection Sharing. PC terhubung dengan kabel LAN ke router/AP, sedangkan HP
terhubung ke Wi-Fi router tersebut. Jangan memasukkan IP Wi-Fi PC
`192.168.10.x` sebagai alamat receiver; jalur HP menggunakan
`192.168.137.1`.

Untuk membuka konfigurasi router gunakan:

```text
http://192.168.137.223/
```

## 3. Prasyarat PC

- Windows PowerShell.
- Python tersedia sebagai perintah `python`.
- CMake dan MinGW-w64 untuk receiver.
- Android SDK/ADB dan JDK 17 untuk APK.
- FFmpeg di `PATH` atau di:

```text
C:\Tools\ffmpeg\bin\ffmpeg.exe
```

- HP dan PC berada pada subnet `192.168.137.0/24`.
- Windows Firewall mengizinkan UDP 8200 dan 8201 pada jaringan rig.

Jika PowerShell menolak menjalankan `.ps1`, gunakan pola berikut:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script.ps1>
```

Opsional, agar karakter terminal tidak rusak:

```powershell
chcp 65001
```

## 4. Build receiver PC

Dari root repository:

```powershell
cd C:\Users\user\ucvserver

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\experiment\build-receiver.ps1
```

Gunakan build bersih jika toolchain atau header wire berubah:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\experiment\build-receiver.ps1 -Clean
```

Build dianggap sehat jika:

```text
100% tests passed, 0 tests failed out of 2
```

Binary utama berada di:

```text
experiment\receiver\build\ucv-receiver.exe
```

## 5. Build dan install aplikasi HP

### 5.1 Wireless debugging (direkomendasikan)

Port USB HP dipakai kamera, jadi gunakan wireless debugging. Di HP buka:

```text
Developer options -> Wireless debugging
```

Jika PC belum dipasangkan, pilih **Pair device with pairing code**, lalu:

```powershell
adb pair <IP-HP>:<PAIRING-PORT>
```

Masukkan kode enam digit. Setelah paired, sambungkan ke alamat debug utama yang
ditampilkan HP:

```powershell
adb connect 192.168.137.139:<DEBUG-PORT>
adb devices
```

Contoh endpoint yang pernah digunakan rig ini:

```text
192.168.137.139:44447
```

Port wireless debugging dapat berubah setelah dimatikan atau reboot; selalu
ikuti port yang sedang ditampilkan HP.

### 5.2 Build APK

Setelah perubahan Kotlin saja:

```powershell
.\gradlew.bat :app:assembleDebug
```

Setelah perubahan native C/JNI, selalu clean build karena CMake incremental
pernah memasukkan `.so` lama:

```powershell
.\gradlew.bat :app:clean :app:assembleDebug
```

APK berada di:

```text
app\build\outputs\apk\debug\app-debug.apk
```

Install melalui Wi-Fi tanpa melepas kamera:

```powershell
adb -s 192.168.137.139:<DEBUG-PORT> install -r `
  .\app\build\outputs\apk\debug\app-debug.apk
```

Jalankan aplikasi:

```powershell
adb -s 192.168.137.139:<DEBUG-PORT> shell am start `
  -n com.anjas.uvcserver/.MainActivity
```

Alternatif saat HP terhubung USB ke PC:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\experiment\install-phone.ps1 -Clean
```

Setelah itu cabut HP dari PC dan pasang kamera melalui OTG.

## 6. Dua mode UI aplikasi HP

Aplikasi mempunyai dua menu.

### Website / Auto

Ini mode normal yang direkomendasikan. UI hanya menampilkan:

- `Open USB Camera`;
- `Close`;
- status camera/control agent;
- status pipeline;
- statistik pipeline;
- log native.

Setelah menekan `Open USB Camera`, aplikasi otomatis membuka control agent pada
UDP 8200. Width, Height, FPS, PC IP, Start Control, dan Start Raw UDP tidak perlu
diisi dari HP.

Status yang diharapkan:

```text
Camera agent ready on 192.168.137.139:8200 - choose mode on PC
Control: READY  192.168.137.139:8200
Pipeline: waiting for website
```

### Manual / Debug

Gunakan hanya untuk diagnosis atau fallback. Menu ini menampilkan kontrol lama:

- Width / Height / FPS;
- PC IP receiver;
- Start/Stop MJPEG Server;
- Start/Stop Control;
- Start/Stop Raw UDP;
- run ID dan status manual.

Jangan menjalankan pipeline manual bersamaan dengan pengukuran website. Hanya
satu UVC stream boleh aktif pada satu waktu.

## 7. Menjalankan dashboard

Pastikan receiver sudah dibangun, lalu:

```powershell
cd C:\Users\user\ucvserver

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\experiment\run-dashboard.ps1
```

Browser membuka:

```text
http://127.0.0.1:8088/
```

Dashboard tidak melakukan hot reload. Setelah kode HTML/Python berubah:

1. hentikan dashboard dengan `Ctrl+C`;
2. jalankan kembali script;
3. tekan `Ctrl+F5` di browser.

## 8. Prosedur pengukuran Website / Auto

### Langkah 1 — Siapkan rig

- PC Ethernet tetap `192.168.137.1`.
- HP tetap `192.168.137.139` dan gateway `192.168.137.1`.
- Kamera terpasang ke HP via OTG.
- Layar HP tetap menyala.
- Untuk perbandingan serius, gunakan daya/baterai dan posisi kamera/router yang
  konsisten.

### Langkah 2 — Buka camera agent

Di HP:

1. pilih menu **Website / Auto**;
2. tekan **Open USB Camera**;
3. izinkan CAMERA dan RECORD_AUDIO jika diminta;
4. izinkan akses USB jika dialog muncul;
5. tunggu `Control: READY`.

HyperOS/MIUI dapat meminta CAMERA/RECORD_AUDIO walaupun capture dilakukan
melalui raw USB host. Izin tersebut tetap harus diberikan.

### Langkah 3 — Ambil resolusi kamera

Di website:

1. pastikan Phone IP adalah `192.168.137.139`;
2. pilih **Raw UDP (H.264)** atau **MJPEG over HTTP** pada Protocol;
3. tekan **Refresh camera modes**;
4. pilih **MJPEG resolution**.

Daftar resolusi dan FPS diambil langsung dari descriptor MJPEG kamera. YUYV/YUV
tidak ikut. Website hanya menampilkan resolusinya; FPS dipilih otomatis sebagai
FPS tertinggi yang kamera iklankan untuk resolusi tersebut.

Contoh:

```text
pilihan UI       : 1920 x 1080
mode internal    : 1920x1080@30
```

Mode internal lengkap tetap disimpan di NDJSON agar perbandingan dapat
direproduksi.

### Langkah 4 — Atur durasi

Rekomendasi awal:

```text
Duration : 60 seconds
Warmup   : 5 seconds
```

Run ID boleh dikosongkan. Dashboard akan membuat `<epoch-milidetik>-run`
otomatis dan mengirim hash yang sama ke HP. Setelah run selesai, field akan
dikosongkan agar Start berikutnya mendapat ID baru. Jika ID lama dimasukkan
lagi, dashboard menambahkan suffix `-2`, `-3`, dan seterusnya; file hasil lama
tidak pernah ditimpa. Receiver CLI juga menolak menimpa file dengan Run ID yang
sudah ada. Ini sekaligus mencegah masalah `wrong-run frames` akibat run ID yang
berbeda.

`Condition` tidak lagi muncul pada form. Run baru otomatis disimpan sebagai
`condition=clean`. Field tetap ada di NDJSON untuk kompatibilitas log lama.

### Langkah 5 — Start

Tekan **Start measurement** satu kali. Untuk Raw UDP urutannya:

```text
clock sync PC <-> HP
-> receiver bind UDP 8201
-> run hash dikirim ke HP
-> START(resolusi + FPS auto) dikirim ke HP
-> HP membuka mode MJPEG terpilih
-> HP decode -> hardware H.264 encode -> Raw UDP
-> PC menerima, mengukur, mencatat, dan menampilkan preview
-> setelah durasi: STOP dikirim ke HP
```

Tidak perlu menekan Start Control atau Start Raw UDP di HP.

Untuk **MJPEG over HTTP**, HP tidak melakukan decode atau re-encode. Kamera
UVC menghasilkan JPEG yang langsung dikirim sebagai HTTP multipart pada port
8181. Header `X-UCV-*` membawa sequence dan timestamp setiap frame.

Console sehat menampilkan:

```text
[clock] offset = ... ms, best RTT = ... ms
[phone] pipeline started remotely: 1280x720@30 -> this PC:8201
[recv] listening...
[recv]    101 frames  transport p50=... ms  jitter=... ms  gaps=0
```

Di HP status berubah menjadi:

```text
Pipeline: STREAMING
```

### Langkah 6 — Selesai atau Stop

Setelah durasi selesai, receiver menulis summary dan mengirim remote STOP.
Tombol **Stop** di website dapat digunakan untuk menghentikan lebih awal. Jangan
menutup paksa terminal receiver jika tidak perlu karena shutdown normal menulis
summary dan menghentikan pipeline HP dengan rapi.

## 9. Pengujian beberapa resolusi

Untuk membandingkan resolusi:

1. mulai dari resolusi rendah;
2. jalankan satu bring-up 30–60 detik;
3. jika sehat, lakukan minimal tiga run valid pada resolusi tersebut;
4. lanjut ke resolusi berikutnya;
5. pertahankan router, posisi, durasi, warmup, dan trafik jaringan tetap sama.

Urutan konservatif:

```text
320x240 -> 640x480 -> 1280x720 -> 1920x1080 -> resolusi lebih tinggi
```

Resolusi tinggi yang diiklankan kamera belum tentu stabil melalui USB
isochronous, decoder, atau hardware encoder HP. Descriptor berarti kamera
mendukung mode tersebut, bukan jaminan seluruh pipeline HP sanggup menjalankan
mode itu tanpa error.

Run hanya valid jika:

- `frames received > 0`;
- `clock_status = OK`;
- frame count terus naik;
- `wrong-run frames = 0`;
- decode/send error nol atau dapat dijelaskan;
- hardware encoder menunjukkan `hw=yes`;
- tidak ada kegagalan reassembly yang signifikan.

## 10. Dashboard dan arti metrik

Dashboard memperbarui run aktif sekitar satu detik sekali. NDJSON di-flush
setiap 10 frame agar grafik realtime tanpa melakukan flush disk untuk setiap
frame.

### Latency

Grafik **Network latency** menggunakan transport latency:

```text
waktu frame diterima PC - waktu frame dikirim HP
```

Nilai satu arah menggunakan clock sync. Perbedaan di bawah kira-kira 5 ms tidak
layak dianggap sebagai pemenang karena residual error sinkronisasi sekitar
±1–3 ms.

### Jitter

Variasi perubahan waktu transit antarpaket/frame, dihitung dengan estimator
RFC 3550. Jitter tidak bergantung pada offset clock absolut.

### Network packet loss / retransmission

Dihitung dari `packet_seq` unik pada setiap datagram Raw UDP:

```text
loss % = missing UDP packets / expected UDP packets * 100
```

Nilai diperbarui realtime dan mendeteksi satu fragmen yang hilang walaupun
frame videonya tidak selesai.

Untuk **MJPEG/TCP**, HP membaca `tcpi_data_segs_out` dan
`tcpi_total_retrans` dari `TCP_INFO` socket yang sedang mengirim. Counter
dikirim pada setiap frame dan receiver menghitung setelah warmup:

```text
TCP retransmission % = retransmitted segments /
                       (data segments + retransmitted segments) * 100
```

Ini memperlihatkan loss jaringan yang dipulihkan oleh TCP dan sebelumnya
terlihat sebagai `0%`. Namanya sengaja **TCP retransmission rate**, karena
retransmission adalah bukti pemulihan transport, bukan frame akhir yang hilang.
Jika perangkat tidak menyediakan `TCP_INFO`, dashboard jatuh kembali ke
observed frame loss dan menandainya dengan label berbeda.

Untuk **RTP/UDP**, dashboard memakai `packet_seq` pada extension RFC 8285 dan
menampilkan **Observed RTP packet loss** realtime. Pilihan `RTP/UDP (H.264)`
saat ini menguji data plane langsung pada UDP 5004. Ia belum menjalankan RTSP
`DESCRIBE/SETUP/PLAY`, sehingga hasil harus diberi nama RTP/UDP, bukan RTSP.

### Statistik lain yang tetap dicatat

Walaupun grafik utama fokus pada latency, jitter, dan loss, NDJSON masih
menyimpan:

- glass-to-glass/capture-to-receive;
- capture-to-encode;
- payload bytes dan goodput;
- control RTT;
- clock offset dan drift;
- gaps, reorder, duplicates, bad header, wrong run, dan reassembly failure.

### Preview

Browser tidak dapat menampilkan Raw H.264 UDP langsung. Receiver tetap menjadi
satu-satunya listener UDP 8201, lalu menyalin frame H.264 lengkap ke channel
localhost. FFmpeg mengubahnya menjadi MJPEG untuk endpoint:

```text
/api/preview.mjpeg
```

Preview tidak mengambil paket langsung dari HP dan tidak berebut port receiver.
Pada protocol MJPEG, receiver meneruskan salinan JPEG langsung ke dashboard;
FFmpeg hanya digunakan untuk preview Raw UDP/H.264.

## 11. File hasil

Semua run ditulis ke:

```text
experiment\results\receiver-<run_id>.ndjson
```

Contoh record:

```json
{"type":"meta","run_id":"...","protocol":"raw_udp","mode":"1280x720@30","condition":"clean"}
{"type":"frame","seq":1,"cap_ns":0,"enc_ns":0,"snd_ns":0,"rcv_ns":0,"bytes":0,"key":true}
{"type":"control","sent":0,"acked":0,"p50_ms":0}
{"type":"summary","frames_received":0,"gap_frames":0,"clock_status":"OK"}
```

Dashboard juga dapat memberi label pada log lama melalui:

```text
experiment\results\.dashboard-labels.json
```

Analisis Markdown manual:

```powershell
python .\experiment\analysis\analyze.py `
  .\experiment\results\*.ndjson --out .\experiment\results\summary.md
```

## 12. Troubleshooting

### `Refresh camera modes` gagal

Penyebab umum:

- belum menekan Open USB Camera;
- izin USB belum diberikan;
- aplikasi baru saja di-install dan control agent belum aktif;
- IP HP berubah;
- dashboard lama belum direstart;
- UDP 8200 diblokir firewall.

Tindakan:

1. buka aplikasi HP;
2. pilih Website / Auto;
3. tekan Close, lalu Open USB Camera;
4. tunggu `Control: READY`;
5. pastikan Phone IP website `192.168.137.139`;
6. restart dashboard dan tekan Refresh camera modes lagi.

### `clock sync failed`

Control agent HP tidak menjawab. Periksa IP HP, status `Control: READY`, subnet,
dan firewall UDP 8200.

### `phone rejected remote START`

Lihat log HP. Penyebab yang pernah muncul:

- mode terlalu berat;
- hardware H.264 encoder menolak resolusi;
- stream USB gagal dimulai;
- pipeline sebelumnya belum berhenti.

Coba resolusi lebih rendah. Jika masih terjadi, Close lalu Open USB Camera.

### `SUBMITURB failed errno=12 (Out of memory)`

Ini berasal dari alokasi transfer USB isochronous Android. Build saat ini
membatasi paket per transfer menjadi 8 untuk mengurangi ukuran URB. Jika masih
muncul:

1. Stop run;
2. Close kamera;
3. cabut/pasang ulang kamera jika perlu;
4. Open kamera;
5. pilih resolusi lebih rendah.

### Pipeline berjalan tetapi `cap=0`

Stream UVC bernegosiasi tetapi tidak menghasilkan frame. Biasanya mode terlalu
berat atau transfer USB stall. Pilih resolusi lebih rendah dan ulangi.

### Receiver menerima 0 frame dan `wrong-run frames` besar

Run ID/hash HP dan PC berbeda. Alur website terbaru mengatur hash otomatis;
masalah ini biasanya berarti dashboard/receiver lama masih berjalan. Restart
dashboard, jangan mulai Raw UDP manual, dan mulai run baru.

### Preview hitam

Periksa console dan summary:

- jika `frames received = 0`, masalah ada sebelum decoder preview;
- jika preview status `waiting`, H.264 belum diterima;
- jika status `error`, periksa FFmpeg;
- jika frame bertambah tetapi gambar benar-benar hitam, periksa output kamera.

### Browser masih menampilkan UI lama

```text
Ctrl+C pada terminal dashboard
jalankan run-dashboard.ps1 lagi
Ctrl+F5 pada browser
```

### Karakter terminal seperti `ΓÇö`

Ini masalah code page terminal, bukan data pengukuran. Jalankan `chcp 65001`
atau gunakan Windows Terminal. NDJSON tetap UTF-8.

### Wireless ADB hilang

Pastikan Wireless debugging aktif, baca port baru, lalu:

```powershell
adb connect 192.168.137.139:<PORT-BARU>
adb devices
```

## 13. Validasi cepat sebelum mengambil data resmi

Lakukan satu run `320x240` selama 30–60 detik. Verifikasi:

```text
preview = live
frames terus bertambah
gaps = 0 atau sangat kecil
wrong-run = 0
clock_status = OK
hw = yes
cap ~= dec ~= enc ~= sent
```

Run pertama setelah install/reconnect dianggap **bring-up**, bukan hasil resmi.
Gunakan run berikutnya untuk perbandingan.

## 14. Handoff untuk Claude

Bagian kode utama:

| Area | File |
|---|---|
| Android UI dan USB permission | `app/src/main/java/com/anjas/uvcserver/MainActivity.kt` |
| JNI surface | `app/src/main/java/com/anjas/uvcserver/UvcNative.kt` |
| Native camera/mode/lifecycle | `app/src/main/cpp/jni_bridge.c` |
| Control agent HP | `app/src/main/cpp/ucv_control.c` |
| Decode/encode/send pipeline | `app/src/main/cpp/ucv_pipeline.c` |
| Hardware H.264 encoder | `app/src/main/cpp/ucv_encoder.c` |
| Raw UDP transport | `app/src/main/cpp/ucv_transport.c` |
| Shared wire contract | `experiment/receiver/include/ucv_wire.h` |
| PC receiver | `experiment/receiver/src/main.cpp` |
| PC network/control client | `experiment/receiver/src/ucv_net.cpp` |
| Dashboard backend | `experiment/dashboard/server.py` |
| Dashboard frontend | `experiment/dashboard/index.html` |

Perilaku penting build saat ini:

- HP otomatis memulai control agent setelah Open USB Camera.
- Website membuat run ID dan mengendalikan remote Start/Stop.
- GET_MODE hanya mengembalikan mode MJPEG dari kamera.
- Website mengelompokkan kombinasi mode menjadi resolusi aktual + daftar FPS.
- UI hanya meminta resolusi; FPS internal adalah nilai tertinggi yang kamera
  iklankan untuk resolusi tersebut.
- Condition form disembunyikan dan run baru memakai `clean`.
- Receiver adalah satu-satunya listener UDP 8201.
- Grafik run aktif diperbarui realtime dari NDJSON.
- Jangan mengedit source vendored libuvc/libusb kecuali masalah memang berasal
  dari sana; repository memiliki perubahan diagnostik USB yang disengaja.
- Setelah perubahan JNI/native, gunakan clean build untuk menghindari stale
  `libuvcserver.so`.

Status verifikasi terakhir:

- receiver berhasil dibangun;
- 2/2 receiver self-test lulus;
- APK berhasil dibangun;
- APK berhasil dipasang melalui wireless ADB;
- preview H.264 -> FFmpeg -> MJPEG telah diuji;
- streaming nyata pernah berhasil dengan frame, latency, jitter, goodput, dan
  zero sequence gaps tercatat.
