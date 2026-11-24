# Smart Pet Feeder

Sistem pemberi makan hewan peliharaan otomatis berbasis IoT (ESP32, MQTT over TLS, Node-RED, InfluxDB, Telegram).
Projek akhir Internet of Things (CIT60029), Universitas Brawijaya.

**Status:** work in progress (simulasi Wokwi, hardware menyusul)

## Fitur
- Pemberian makan otomatis terjadwal + manual dari dashboard
- Pantau stok makanan (ultrasonik) dan ukur porsi (load cell)
- Edge: jadwal & keamanan feeding tetap jalan saat internet putus
- Notifikasi Telegram: stok menipis / feeding gagal
- Komunikasi MQTT over TLS (8883), kredensial terpisah dari repo

## Struktur
```
firmware/smart_pet_feeder/   kode ESP32 (Arduino)
wokwi/                       diagram.json + libraries.txt
node-red/flows.json          flow placeholder
docs/                        arsitektur, topik MQTT, ancaman, langkah Wokwi
```

## Mulai cepat
1. `cp firmware/smart_pet_feeder/secrets.h.example firmware/smart_pet_feeder/secrets.h` lalu isi.
2. Isi `ca_cert.h` dan set `TLS_VERIFY_CA 1` untuk versi final.
3. Simulasi: lihat `docs/wokwi-setup.md`.

## Pin
| Komponen | GPIO |
|---|---|
| HC-SR04 TRIG / ECHO | 5 / 18 |
| HX711 DT / SCK | 19 / 21 |
| Servo | 13 |

## Roadmap
- [x] Diagram arsitektur
- [x] Firmware awal + simulasi Wokwi
- [ ] Broker TLS + CA terverifikasi
- [ ] Node-RED dashboard + InfluxDB
- [ ] Telegram bot
- [ ] Kalibrasi load cell, uji hardware
- [ ] Analisis time-series, retensi, threat model final
- [ ] Packaging fisik + video demo
