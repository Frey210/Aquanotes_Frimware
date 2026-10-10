# Panduan kode LCD Aquanotes MQTT

Berlaku untuk firmware MQTT `v2.0.0` sampai `v2.2.x`. Status atas memakai format:

```text
W:<Wi-Fi> S:<pengiriman> B:<baterai>
```

Kondisi normal portable: `W:OK S:OK B:80% 16,0V`. Kondisi normal panel: `W:OK S:OK B:DC`.

## Wi-Fi (`W:`)

| Kode | Arti | Periksa |
|---|---|---|
| `W:OK` | Terhubung dan mendapat IP. | Lanjut lihat `S:` bila data belum masuk. |
| `W:E0` | Idle/mulai koneksi. | Tunggu auto-reconnect. |
| `W:E1` | SSID tidak ditemukan. | AP, jangkauan, SSID, dan Wi-Fi 2,4 GHz. |
| `W:E2` | Scan selesai, belum terhubung. | Tunggu atau buka portal Wi-Fi. |
| `W:E4` | Koneksi/otorisasi gagal. | Password, keamanan AP, pembatasan MAC. |
| `W:E5` | Koneksi aktif terputus. | Sinyal, AP, interferensi, daya ESP32. |
| `W:E6` | Wi-Fi terputus. | `Menu > WiFi Manager > Start Portal`. |
| `W:E-1` | Radio Wi-Fi tidak siap. | Restart; periksa suplai/flash bila berulang. |

Auto-reconnect dimulai 15 detik setelah boot lalu dicoba tiap 30 detik.

## Pengiriman MQTT (`S:`)

| Kode | Arti | Periksa |
|---|---|---|
| `S:OK` | Broker menerima publish QoS 1 dan mengirim PUBACK. | Normal. Backend seharusnya menerima data. |
| `S:--` | Belum mencoba atau Wi-Fi belum terhubung. | `W:`, lalu tunggu interval 10 detik. |
| `S:ECFG` | UID/credential MQTT belum diprovision ke NVS. | Hubungkan USB dan jalankan Aquanotes Firmware Uploader; jangan membuat UID baru bila device sudah terdaftar. |
| `S:ECON` | WSS/TLS/MQTT connect atau autentikasi gagal. | Internet/DNS, port 443, route Cloudflare `/mqtt`, broker, UID/password, CA, lalu Serial Monitor. |
| `S:EPUB` | Terhubung tetapi publish/PUBACK gagal. | ACL topic `devices/<UID>/telemetry`, koneksi, broker/backend, lalu Serial Monitor. |

Serial Monitor 115200 menampilkan `[MQTT] connect gagal: err=... rc=...` atau `[MQTT] publish gagal: ...`. Credential tidak pernah dicetak.

## Baterai (`B:`)

| Kode | Arti | Periksa |
|---|---|---|
| `B:<n>% <v>V` | ADC valid; estimasi baterai 4S. | Bandingkan voltase dengan multimeter. |
| `B:DC` | Unit berprofil panel memakai power supply langsung; ADC baterai sengaja tidak dibaca. | Normal untuk unit panel. Jika unit sebenarnya portable, provision ulang dengan profil `portable`. |
| `B:0% <v>V` | Tegangan pada/di bawah kurva 13,2 V. | Baterai, pembagi 100k/22k, ground, GPIO1. |
| `B:100% <v>V` | Tegangan pada/di atas 16,8 V. | Pastikan tidak over-voltage. |
| `B:EADC` | ADC di bawah 0,1 V/tidak valid. | GPIO1, resistor, solderan, ground, konektor. |

Masalah baterai tidak menghentikan publish telemetry.

## OTA firmware

OTA tidak ditampilkan sebagai error sensor. Status lengkap terlihat di halaman admin firmware dan Serial Monitor.

| Kode | Arti | Periksa |
|---|---|---|
| `LOW_BATTERY` | Unit portable: baterai tidak valid atau di bawah 50%. Profil panel tidak memakai gate ini. | Nyalakan dari sumber normal; cek `B:` dan rangkaian GPIO1. Jangan ubah portable menjadi panel hanya untuk melewati proteksi. |
| `WEAK_WIFI` | RSSI di bawah -75 dBm. | Dekatkan AP/perbaiki antena lalu tunggu retry. |
| `WIFI_OFFLINE` | Wi-Fi tidak terhubung. | Ikuti pemeriksaan `W:`. |
| `CALIBRATION_ACTIVE` | Kalibrasi sensor sedang berjalan. | Selesaikan/keluar dari wizard kalibrasi. |
| `HTTP_*` | Download artifact gagal. | Domain API, Cloudflare, token assignment, dan log backend. |
| `HASH_MISMATCH` | SHA-256 artifact tidak cocok. | Retire release; jangan mencoba artifact yang sama lagi. |
| `SIGNATURE_INVALID` | Signature ECDSA tidak valid. | Signing key/key ID dan pasangan `.bin`/`.sig`. |
| `IMAGE_BEGIN_FAILED` / `IMAGE_WRITE_FAILED` / `IMAGE_END_FAILED` | Penulisan slot OTA gagal. | Flash/partisi, ukuran artifact, suplai daya; gunakan recovery USB bila berulang. |
| `NVS_FAILED` | Status pending OTA gagal disimpan. | NVS/flash; recovery melalui USB. |
| `VERSION_REJECTED` | Versi lebih lama dikirim tanpa flag rollback. | Pilih release lebih baru atau gunakan rollback resmi. |

Urutan normal adalah `assigned`, `downloading`, `downloaded`, `rebooting`, `pending_verify`, lalu `success`. `deferred` berarti perangkat aman tetapi menunggu syarat terpenuhi; `rolled_back` berarti bootloader kembali ke firmware known-good.

## Nilai sensor `--`

| Kotak | Arti | Periksa |
|---|---|---|
| `Temp` | Tidak ada temperatur valid. | Sensor aktif dan komunikasi RS485. |
| `pH` | Pembacaan pH gagal. | Daya, A/B, ground, baud 9600, slave ID `3`. |
| `DO` | Pembacaan DO gagal. | Daya, A/B, ground, baud 9600, slave ID `10`. |
| `EC` / `TDS` / `Salinity` | Sensor EC gagal. | Daya, A/B, ground, baud 9600, slave ID `4`. |

Sensor yang gagal/tidak dipasang menjadi `null` pada payload. Nilai sensor lain tetap dikirim. Nilai angka `0` tetap dianggap data sah bila pembacaan sensor berhasil. NH4 saat ini nonaktif dan dikirim `null`.

## Pesan lain dan LED

| Tampilan/LED | Arti |
|---|---|
| `WiFi: OK` / `WiFi: --` | Status Wi-Fi saat splash. |
| `SEND: OK` / `SEND: --` | Sudah/belum ada PUBACK MQTT. |
| `Radio aktif / Tunggu koneksi...` | Portal Wi-Fi sedang aktif. |
| Hijau tetap | Wi-Fi terhubung, tanpa error berulang. |
| Cyan singkat | Publish baru berhasil. |
| Biru lambat | Wi-Fi belum terhubung. |
| Merah cepat | Sensor gagal berulang atau status `S:E...` berulang. |
| Magenta singkat | Kalibrasi berhasil. |

## Data untuk eskalasi

Kirim foto seluruh LCD, log Serial Monitor sejak boot hingga dua interval publish, UID dan versi firmware, SSID, waktu kejadian, serta hasil cek `https://mqtt.farlabs.my.id/`. Jangan kirim password MQTT melalui chat/log publik.
