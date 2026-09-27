# Aquanotes Firmware MQTT

Firmware ESP32-S3 Aquanotes Triangle dengan TFT ILI9341, empat tombol, sensor RS485, indikator baterai, dan pengiriman telemetry melalui MQTT over secure WebSocket.

## Kontrak MQTT

- Broker: `mqtt.farlabs.my.id`
- Transport: WSS port `443`, path `/mqtt`, subprotocol `mqtt`
- Topic: `devices/{DEVICE_UID}/telemetry`
- Auth: username sama dengan `DEVICE_UID`, password unik per perangkat
- QoS: `1`; LCD menampilkan `S:OK` hanya setelah PUBACK diterima
- Interval: 10 detik
- Payload: `message_id`, `uid`, `suhu`, `ph`, `do`, `tds`, `ammonia`, `salinitas`
- Sensor yang tidak tersedia dikirim sebagai JSON `null`; sensor lain tetap dikirim.
- Timestamp tidak dikirim oleh alat. Backend menentukan waktu penerimaan.

NTP berjalan asinkron hanya untuk memperbaiki jam TLS. Firmware tidak menunggu NTP dan pembacaan sensor tidak pernah ditahan oleh status NTP.

## Credential lokal

`include/secrets.h` sengaja di-ignore oleh Git. Buat dari credential lokal backend:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ..\scripts\sync_local_secrets.ps1
```

Untuk perangkat lain, salin `include/secrets.example.h` menjadi `include/secrets.h`, isi UID dan password MQTT yang sudah diprovisikan di broker/backend.

## Build, upload, monitor

Jalankan dari folder `aquanotes-triangle`:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

Hasil build:

- `.pio/build/esp32-s3-devkitc-1/firmware.bin`
- `.pio/build/esp32-s3-devkitc-1/firmware.factory.bin`

## Konfigurasi perangkat saat ini

- Board: ESP32-S3 DevKitC-1
- pH slave ID `3`, aktif
- EC/TDS/salinitas slave ID `4`, aktif
- NH4 slave ID `1`, nonaktif
- DO slave ID `10`, aktif
- Wi-Fi disimpan oleh ESP32; portal konfigurasi: `Aerasea-Setup`

## Pin utama

| Fungsi | GPIO |
|---|---:|
| TFT CS / DC / RST | 10 / 9 / 8 |
| TFT MOSI / MISO / SCK | 11 / 13 / 12 |
| TFT backlight | 21 |
| Tombol UP / DOWN / OK / BACK | 5 / 6 / 7 / 17 |
| RS485 RX / TX / DE-RE | 16 / 15 / 14 |
| Battery ADC | 1 |
| LED R / G / B | 2 / 3 / 4 |

Panduan diagnosis LCD ada di [`../ref/error_code.md`](../ref/error_code.md).
