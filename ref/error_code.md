# Panduan kode LCD Aquanotes MQTT

Berlaku untuk firmware `v2.0.0-MQTT`. Status atas memakai format:

```text
W:<Wi-Fi> S:<pengiriman> B:<baterai>
```

Kondisi normal: `W:OK S:OK B:80% 16,0V`.

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
| `S:ECON` | WSS/TLS/MQTT connect atau autentikasi gagal. | Internet/DNS, port 443, route Cloudflare `/mqtt`, broker, UID/password, CA, lalu Serial Monitor. |
| `S:EPUB` | Terhubung tetapi publish/PUBACK gagal. | ACL topic `devices/<UID>/telemetry`, koneksi, broker/backend, lalu Serial Monitor. |

Serial Monitor 115200 menampilkan `[MQTT] connect gagal: err=... rc=...` atau `[MQTT] publish gagal: ...`. Credential tidak pernah dicetak.

## Baterai (`B:`)

| Kode | Arti | Periksa |
|---|---|---|
| `B:<n>% <v>V` | ADC valid; estimasi baterai 4S. | Bandingkan voltase dengan multimeter. |
| `B:0% <v>V` | Tegangan pada/di bawah kurva 13,2 V. | Baterai, pembagi 100k/22k, ground, GPIO1. |
| `B:100% <v>V` | Tegangan pada/di atas 16,8 V. | Pastikan tidak over-voltage. |
| `B:EADC` | ADC di bawah 0,1 V/tidak valid. | GPIO1, resistor, solderan, ground, konektor. |

Masalah baterai tidak menghentikan publish telemetry.

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
