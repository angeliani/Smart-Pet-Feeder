#pragma once
// =====================================================================
// config.h - semua parameter NON-rahasia ada di sini (aman di-commit)
// Nilai bertanda [KALIBRASI] harus diukur ulang saat hardware sudah ada.
// =====================================================================

// ---- Identitas perangkat ----
#define DEVICE_ID "feeder-01"

// ---- Pin (GPIO ESP32) ----
#define PIN_TRIG    5    // HC-SR04 TRIG
#define PIN_ECHO    18   // HC-SR04 ECHO (hardware asli: pakai pembagi tegangan 5V -> 3.3V!)
#define PIN_HX_DT   19   // HX711 DT
#define PIN_HX_SCK  21   // HX711 SCK
#define PIN_SERVO   13   // Servo signal (daya servo dari adaptor eksternal, GND disatukan)

// ---- Kalibrasi sensor ----
#define HX711_CAL_FACTOR 420.0f  // [KALIBRASI] placeholder; hitung pakai beban referensi
#define BOWL_EMPTY_CM    20.0f   // [KALIBRASI] jarak sensor ke dasar wadah saat KOSONG
#define BOWL_FULL_CM      4.0f   // [KALIBRASI] jarak sensor ke permukaan saat PENUH
#define LOW_STOCK_PCT    20      // di bawah ini -> event low_stock

// ---- Servo / katup ----
#define SERVO_CLOSED_DEG 0
#define SERVO_OPEN_DEG   90

// ---- Porsi & keamanan fisik (edge safety) ----
#define DEFAULT_PORTION_G 30     // gram per pemberian makan
#define FEED_TIMEOUT_MS   8000   // batas waktu katup terbuka
#define MIN_RATIO_OK      0.8f   // delivered >= 80% target -> dianggap OK
#define MIN_FEED_GAP_MS   10000  // jeda minimum antar feeding (produksi: mis. 300000)
#define MAX_DAILY_GRAMS   150    // batas harian, mencegah overfeeding (mis. akibat perintah palsu)

// ---- Jadwal lokal (jam WIB, dieksekusi di ESP32 walau internet putus) ----
static const uint8_t FEED_HOURS[] = {7, 17};
#define TZ_OFFSET_SEC (7 * 3600)

// ---- Edge: report-by-exception ----
#define SAMPLE_INTERVAL_MS 2000   // baca sensor tiap 2 dtk
#define HEARTBEAT_MS       60000  // kirim paksa tiap 60 dtk
#define DELTA_WEIGHT_G     2.0f   // kirim jika berat berubah >= 2 g
#define DELTA_STOCK_PCT    3.0f   // kirim jika stok berubah >= 3 %

// ---- MQTT ----
#define MQTT_PORT 8883            // TLS
// 1 = verifikasi sertifikat CA broker (WAJIB untuk versi final)
// 0 = sementara untuk simulasi sebelum ca_cert.h diisi
#define TLS_VERIFY_CA 0
