# Arsitektur Smart Pet Feeder

```mermaid
flowchart TB
  subgraph P[Perception]
    S1[HC-SR04: stok]; S2[Load Cell + HX711: porsi]; A1[Servo MG996R: katup]
  end
  subgraph E[Edge - ESP32]
    L[Jadwal lokal, cek stok, ukur porsi, batas harian, report-by-exception]
  end
  C[Connectivity: MQTT over TLS :8883]
  subgraph CL[Cloud]
    N[Node-RED] --> DB[(InfluxDB)]
  end
  subgraph AP[Application]
    D[Dashboard]; T[Telegram Bot]
  end
  P --> E --> C --> CL --> AP
  AP -. cmd feed/tare .-> C -.-> E
```

> Catatan konsistensi: Dokumen Perancangan memakai **Node-RED + InfluxDB**, sedangkan diagram arsitektur
> sebelumnya menulis ThingsBoard/MongoDB. Putuskan satu stack lalu samakan diagramnya.

## Topik MQTT
| Topik | Arah | Isi |
|---|---|---|
| `petfeeder/<id>/telemetry` | device -> cloud | stock_pct, bowl_g, distance_cm, daily_g, rssi, samples, sent |
| `petfeeder/<id>/event` | device -> cloud | feed (OK/FAILED/REJECTED), low_stock |
| `petfeeder/<id>/status` | device -> cloud | online/offline (LWT, retained) |
| `petfeeder/<id>/cmd` | cloud -> device | `{"action":"feed","grams":30}` / `{"action":"tare"}` |

## Keputusan edge
| Keputusan | Kenapa di edge | Jalan saat offline? |
|---|---|---|
| Jadwal makan | hewan tetap diberi makan walau internet mati | Ya (NTP sekali, lalu RTC) |
| Hentikan servo saat porsi tercapai / timeout | respons cepat, aman | Ya |
| Batas harian + jeda minimum | cegah overfeeding | Ya |
| Low stock (histeresis) | tidak spam notifikasi | Ya (event dibuffer) |
| Report-by-exception | hemat pesan | Ya |

## Perbandingan pesan sebelum/sesudah edge (isi saat uji)
| Skenario | Sampel sensor | Pesan terkirim | Penghematan |
|---|---|---|---|
| Tanpa edge (kirim tiap sampel) | _isi_ | _isi_ | - |
| Dengan edge | _isi_ | _isi_ | _isi_ % |

(Ambil dari field `samples` dan `sent` pada telemetry.)

## Analisis ancaman (draf)
| Ancaman | Dampak | Mitigasi |
|---|---|---|
| Perintah feed palsu | Overfeeding | TLS, user/pass unik per perangkat, ACL topik cmd, MAX_DAILY_GRAMS |
| Sadap lalu lintas | Data jadwal/kebiasaan bocor | MQTT over TLS 8883 + verifikasi CA |
| Kredensial bocor di repo | Perangkat diambil alih | `secrets.h` di .gitignore, hanya `secrets.h.example` yang di-commit |
| Telemetri dipalsukan | Notifikasi/analisis menyesatkan | Kredensial per perangkat, ACL topik telemetry |
| Katup macet | Hewan tidak makan | Deteksi FAILED + notifikasi Telegram |
