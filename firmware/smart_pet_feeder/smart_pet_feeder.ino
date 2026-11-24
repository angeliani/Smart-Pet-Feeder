/*
  ============================================================
  SMART PET FEEDER
  ESP32 Firmware
  ============================================================

  Architecture:
    Sensors
       ↓
    Edge Processing
       ↓
    MQTT over TLS :8883
       ↓
    Node-RED
       ↓
    MongoDB + Dashboard + Telegram

  Sensors:
    1. HC-SR04      -> food stock estimation
    2. HX711        -> food weight / portion measurement

  Actuator:
    1. Servo        -> food dispensing gate

  MQTT Topics:
    petfeeder/{DEVICE_ID}/telemetry
    petfeeder/{DEVICE_ID}/event
    petfeeder/{DEVICE_ID}/status
    petfeeder/{DEVICE_ID}/cmd

  Commands:
    Feed:
      {"action":"feed","grams":30}

    Tare:
      {"action":"tare"}

  Libraries:
    - PubSubClient
    - ArduinoJson v7
    - ESP32Servo
    - HX711
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <time.h>
#include <math.h>

#include "config.h"
#include "secrets.h"
#include "ca_cert.h"

// ============================================================
// OBJECTS
// ============================================================
WiFiClientSecure net;
PubSubClient mqtt(net);

HX711 scale;
Servo gate;

// ============================================================
// MQTT TOPICS
// ============================================================
char topicTelemetry[64];
char topicEvent[64];
char topicStatus[64];
char topicCmd[64];

// ============================================================
// SYSTEM STATE
// ============================================================
// Telemetry
static float lastPubStock = -100.0f;
static float lastPubWeight = -1000.0f;

static uint32_t lastPubMs = 0;
static uint32_t lastSampleMs = 0;

// MQTT / WiFi
static uint32_t lastMqttTry = 0;
static uint32_t lastWifiTry = 0;

// Feeding
static uint32_t lastFeedMs = 0;

// Counters
static uint32_t samplesTaken = 0;
static uint32_t msgsSent = 0;

// Daily feeding
static float dailyGrams = 0.0f;
static int lastYday = -1;
static int lastFeedKey = -1;

// Low stock state
static bool lowStockFlag = false;

// Pending commands
static volatile bool pendingFeed = false;
static volatile bool pendingTare = false;

static float pendingFeedG = DEFAULT_PORTION_G;

// ============================================================
// EVENT RING BUFFER
// ============================================================
#define EVT_BUF 10
#define EVT_LEN 300

static char evtBuf[EVT_BUF][EVT_LEN];
static uint8_t evtHead = 0;
static uint8_t evtCount = 0;

// ============================================================
// EVENT BUFFER FUNCTIONS
// ============================================================
static void queueEvent(const char* json) {
  // Jika buffer penuh, buang event paling lama
  if (evtCount == EVT_BUF) {
    evtHead = (evtHead + 1) % EVT_BUF;
    evtCount--;
  }

  uint8_t index = (evtHead + evtCount) % EVT_BUF;
  strlcpy(evtBuf[index], json, EVT_LEN);
  evtCount++;
}

static void flushEvents() {
  while (evtCount > 0 && mqtt.connected()) {
    if (!mqtt.publish(topicEvent, evtBuf[evtHead], false)) {
      // Publish gagal.
      // Jangan hapus event supaya bisa dicoba lagi.
      break;
    }

    evtHead = (evtHead + 1) % EVT_BUF;
    evtCount--;
    msgsSent++;
  }
}

// ============================================================
// TIME
// ============================================================
static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  if (t > 1600000000) {
    return (uint32_t)t;
  }
  // 0 = waktu belum valid
  return 0;
}

// ============================================================
// EVENT GENERATOR
// ============================================================
static void emitEvent(
  const char* type,
  const char* status,
  float targetG,
  float deliveredG,
  const char* reason,
  const char* trigger = "") {
  JsonDocument doc;
  doc["ts"] = nowEpoch();
  doc["device"] = DEVICE_ID;
  doc["type"] = type;
  doc["status"] = status;
  doc["target_g"] = targetG;
  doc["delivered_g"] = deliveredG;
  doc["reason"] = reason;
  doc["trigger"] = trigger;

  char buf[EVT_LEN];
  serializeJson(doc, buf, sizeof(buf));

  Serial.printf("[EVENT] %s\n", buf);
  queueEvent(buf);
}

// ============================================================
// WIFI
// ============================================================
static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWifiTry < 5000) {
    return;
  }

  lastWifiTry = millis();
  Serial.println("[WiFi] mencoba koneksi...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================
// SENSOR - HC-SR04
// ============================================================
static float readDistanceCm() {
  float valid[3];
  int n = 0;

  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long us = pulseIn(PIN_ECHO, HIGH, 30000);
    if (us > 0) {
      valid[n++] = us * 0.0343f / 2.0f;
    }

    delay(30);
  }

  // Tidak ada pembacaan valid
  if (n == 0) {
    return NAN;
  }

  // Sort untuk mengambil median
  for (int i = 1; i < n; i++) {
    float key = valid[i];
    int j = i - 1;

    while (j >= 0 && valid[j] > key) {
      valid[j + 1] = valid[j];
      j--;
    }

    valid[j + 1] = key;
  }

  return valid[n / 2];
}

// ============================================================
// STOCK PERCENTAGE
// ============================================================
static float stockPercent(float cm) {
  if (isnan(cm)) {
    return -1;
  }

  float denominator = BOWL_EMPTY_CM - BOWL_FULL_CM;
  if (denominator <= 0) {
    return -1;
  }

  float pct = ((BOWL_EMPTY_CM - cm) / denominator) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

// ============================================================
// SENSOR - HX711
// ============================================================
static float readWeightG() {
  if (!scale.is_ready()) {
    return NAN;
  }

  float weight = scale.get_units(3);

  // Hindari nilai negatif akibat noise kecil
  if (weight < 0 && weight > -5) {
    weight = 0;
  }

  return weight;
}

// ============================================================
// FEEDING
// ============================================================
static void feedPortion(float targetG, const char* trigger) {
  uint32_t now = millis();

  // ----------------------------------------------------------
  // Validasi target
  // ----------------------------------------------------------
  targetG = constrain(targetG, 5.0f, 60.0f);

  // ----------------------------------------------------------
  // Minimum feeding gap
  // ----------------------------------------------------------
  if (lastFeedMs != 0 && now - lastFeedMs < MIN_FEED_GAP_MS) {
    Serial.println("[FEED] ditolak: terlalu cepat");
    emitEvent("feed", "REJECTED", targetG, 0, "too_soon", trigger);
    return;
  }

  // ----------------------------------------------------------
  // Daily limit
  // ----------------------------------------------------------
  if (dailyGrams >= MAX_DAILY_GRAMS) {
    Serial.println("[FEED] ditolak: daily limit");
    emitEvent("feed", "REJECTED", targetG, 0, "daily_limit", trigger);
    return;
  }

  // ----------------------------------------------------------
  // Jangan biarkan target melewati sisa daily limit
  // ----------------------------------------------------------
  float remaining = MAX_DAILY_GRAMS - dailyGrams;
  if (targetG > remaining) {
    Serial.println("[FEED] ditolak: target melebihi sisa daily limit");
    emitEvent("feed", "REJECTED", targetG, 0, "target_exceeds_daily_limit", trigger);
    return;
  }

  Serial.printf("[FEED] mulai | target %.1f g | trigger=%s\n", targetG, trigger);

  // ----------------------------------------------------------
  // Ambil berat awal
  // ----------------------------------------------------------
  float startWeight = readWeightG();
  if (isnan(startWeight)) {
    Serial.println("[FEED] HX711 tidak tersedia");
    emitEvent("feed", "FAILED", targetG, 0, "hx711_not_ready", trigger);
    return;
  }

  // ----------------------------------------------------------
  // Buka servo
  // ----------------------------------------------------------
  gate.write(SERVO_OPEN_DEG);

  uint32_t startMs = millis();
  float delivered = 0;

  // ----------------------------------------------------------
  // Feeding loop
  // ----------------------------------------------------------
  while (millis() - startMs < FEED_TIMEOUT_MS) {
    delay(100);
    float currentWeight = readWeightG();

    if (!isnan(currentWeight)) {
      delivered = currentWeight - startWeight;
      if (delivered < 0) {
        delivered = 0;
      }
    }

    // Target tercapai
    if (delivered >= targetG) {
      break;
    }
  }

  // ----------------------------------------------------------
  // Tutup servo
  // ----------------------------------------------------------
  gate.write(SERVO_CLOSED_DEG);
  lastFeedMs = millis();

  // ----------------------------------------------------------
  // Evaluasi hasil
  // ----------------------------------------------------------
  bool ok = delivered >= targetG * MIN_RATIO_OK;

  if (ok) {
    // Gunakan berat aktual
    dailyGrams += delivered;

    // Safety clamp
    if (dailyGrams > MAX_DAILY_GRAMS) {
      dailyGrams = MAX_DAILY_GRAMS;
    }

    Serial.printf("[FEED] berhasil | %.1f g\n", delivered);
    emitEvent("feed", "OK", targetG, delivered, "feeding_completed", trigger);
  } else {
    Serial.printf("[FEED] gagal | delivered %.1f g\n", delivered);
    emitEvent("feed", "FAILED", targetG, delivered, "under_delivered_or_jam", trigger);
  }
}

// ============================================================
// SCHEDULER
// ============================================================
static void runScheduler() {
  struct tm t;

  // Waktu belum tersedia
  if (!getLocalTime(&t, 0)) {
    return;
  }

  // ----------------------------------------------------------
  // Reset daily counter ketika hari berganti
  // ----------------------------------------------------------
  if (t.tm_yday != lastYday) {
    lastYday = t.tm_yday;
    dailyGrams = 0;
    lastFeedKey = -1;
    Serial.println("[SCHEDULE] hari baru, daily counter reset");
  }

  // ----------------------------------------------------------
  // Check semua jadwal
  // ----------------------------------------------------------
  for (uint8_t i = 0; i < FEED_HOURS_COUNT; i++) {
    uint8_t h = FEED_HOURS[i];
    int key = t.tm_yday * 24 + h;

    if (t.tm_hour == h && t.tm_min == 0 && key != lastFeedKey) {
      lastFeedKey = key;
      Serial.printf("[SCHEDULE] waktu feeding %02d:00\n", h);
      feedPortion(DEFAULT_PORTION_G, "schedule");
    }
  }
}

// ============================================================
// MQTT MESSAGE CALLBACK
// ============================================================
static void onMessage(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT CMD] topic=%s\n", topic);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, len);

  if (error) {
    Serial.printf("[MQTT CMD] JSON error: %s\n", error.c_str());
    return;
  }

  const char* action = doc["action"] | "";

  // ----------------------------------------------------------
  // FEED COMMAND
  // ----------------------------------------------------------
  if (strcmp(action, "feed") == 0) {
    float grams = doc["grams"] | (float)DEFAULT_PORTION_G;
    grams = constrain(grams, 5.0f, 60.0f);

    pendingFeedG = grams;
    pendingFeed = true;

    Serial.printf("[CMD] feed %.1f g\n", grams);
  }
  // ----------------------------------------------------------
  // TARE COMMAND
  // ----------------------------------------------------------
  else if (strcmp(action, "tare") == 0) {
    pendingTare = true;
    Serial.println("[CMD] tare requested");
  } else {
    Serial.printf("[CMD] unknown action: %s\n", action);
  }
}

// ============================================================
// MQTT CONNECTION
// ============================================================
static void connectMqtt() {
  if (mqtt.connected()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Coba setiap 5 detik
  if (millis() - lastMqttTry < 5000) {
    return;
  }

  lastMqttTry = millis();

  // Client ID unik
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] connecting... ");

  bool connected = mqtt.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS,
    // Last Will topic
    topicStatus,
    // Will QoS
    1,
    // Will retain
    true,
    // Will payload
    "offline");

  if (connected) {
    Serial.println("OK");

    // --------------------------------------------------------
    // Publish online
    // --------------------------------------------------------
    mqtt.publish(topicStatus, "online", true);

    // --------------------------------------------------------
    // Subscribe command
    // --------------------------------------------------------
    if (mqtt.subscribe(topicCmd, 1)) {
      Serial.printf("[MQTT] subscribed: %s\n", topicCmd);
    } else {
      Serial.println("[MQTT] subscribe gagal");
    }

    // --------------------------------------------------------
    // Setelah reconnect, event buffer akan dikirim
    // --------------------------------------------------------
    flushEvents();
  } else {
    Serial.printf("gagal rc=%d\n", mqtt.state());
  }
}

// ============================================================
// TELEMETRY
// ============================================================
static void sampleAndPublish() {
  samplesTaken++;

  // ----------------------------------------------------------
  // Read sensors
  // ----------------------------------------------------------
  float distance = readDistanceCm();
  float stock = stockPercent(distance);
  float weight = readWeightG();

  // ----------------------------------------------------------
  // LOW STOCK EDGE DECISION
  // ----------------------------------------------------------
  if (stock >= 0) {
    // Masuk kondisi low stock
    if (!lowStockFlag && stock < LOW_STOCK_PCT) {
      lowStockFlag = true;
      Serial.printf("[EDGE] LOW STOCK %.1f%%\n", stock);
      emitEvent("low_stock", "ALERT", stock, 0, "stock_below_threshold", "edge");
    }

    // Hysteresis
    if (lowStockFlag && stock > LOW_STOCK_PCT + 10) {
      lowStockFlag = false;
      emitEvent("low_stock", "NORMAL", stock, 0, "stock_recovered", "edge");
    }
  }

  // ----------------------------------------------------------
  // REPORT BY EXCEPTION
  // ----------------------------------------------------------
  bool changed = false;

  // Stock berubah cukup signifikan
  if (stock >= 0 && (lastPubStock < 0 || fabsf(stock - lastPubStock) >= DELTA_STOCK_PCT)) {
    changed = true;
  }

  // Weight berubah cukup signifikan
  if (!isnan(weight) && (lastPubWeight < 0 || fabsf(weight - lastPubWeight) >= DELTA_WEIGHT_G)) {
    changed = true;
  }

  // Heartbeat
  bool heartbeat = (lastPubMs == 0 || millis() - lastPubMs >= HEARTBEAT_MS);

  // Jika tidak berubah dan belum heartbeat,
  // tidak perlu kirim.
  if (!changed && !heartbeat) {
    return;
  }

  // ----------------------------------------------------------
  // TELEMETRY JSON
  // ----------------------------------------------------------
  JsonDocument doc;
  doc["ts"] = nowEpoch();
  doc["device"] = DEVICE_ID;
  doc["distance_cm"] = isnan(distance) ? -1 : distance;
  doc["stock_pct"] = stock;
  doc["bowl_g"] = isnan(weight) ? -1 : weight;
  doc["daily_g"] = dailyGrams;
  doc["rssi"] = WiFi.RSSI();
  doc["samples"] = samplesTaken;
  // Nomor pesan yang akan dikirim
  doc["sent"] = msgsSent + 1;

  char buf[350];
  serializeJson(doc, buf, sizeof(buf));

  Serial.printf("[TELEMETRY] %s\n", buf);

  // ----------------------------------------------------------
  // MQTT PUBLISH
  // ----------------------------------------------------------
  if (mqtt.connected()) {
    bool published = mqtt.publish(topicTelemetry, buf, false);

    if (published) {
      msgsSent++;
      lastPubMs = millis();

      if (stock >= 0) {
        lastPubStock = stock;
      }

      if (!isnan(weight)) {
        lastPubWeight = weight;
      }
    } else {
      Serial.println("[MQTT] telemetry publish gagal");
    }
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("================================");
  Serial.println(" SMART PET FEEDER");
  Serial.println(" ESP32 Firmware");
  Serial.println("================================");

  // ==========================================================
  // PIN SETUP
  // ==========================================================
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  // ==========================================================
  // HX711
  // ==========================================================
  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(HX711_CAL_FACTOR);
  delay(300);
  scale.tare();

  // ==========================================================
  // SERVO
  // ==========================================================
  gate.setPeriodHertz(50);
  gate.attach(PIN_SERVO, 500, 2400);
  gate.write(SERVO_CLOSED_DEG);

  // ==========================================================
  // MQTT TOPICS
  // ==========================================================
  snprintf(topicTelemetry, sizeof(topicTelemetry), "petfeeder/%s/telemetry", DEVICE_ID);
  snprintf(topicEvent, sizeof(topicEvent), "petfeeder/%s/event", DEVICE_ID);
  snprintf(topicStatus, sizeof(topicStatus), "petfeeder/%s/status", DEVICE_ID);
  snprintf(topicCmd, sizeof(topicCmd), "petfeeder/%s/cmd", DEVICE_ID);

  Serial.println("[MQTT] Topics:");
  Serial.println(topicTelemetry);
  Serial.println(topicEvent);
  Serial.println(topicStatus);
  Serial.println(topicCmd);

  // ==========================================================
  // WIFI
  // ==========================================================
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WiFi] connected | IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[WiFi] belum terhubung");
    Serial.println("[WiFi] edge mode tetap berjalan");
  }

  // ==========================================================
  // NTP
  // ==========================================================
  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

  // ==========================================================
  // TLS
  // ==========================================================
#if TLS_VERIFY_CA
  /*
    FINAL / REAL DEVICE

    Sertifikat CA broker digunakan
    untuk memverifikasi broker.
  */
  net.setCACert(ROOT_CA);
#else
  /*
    SIMULATION / WOKWI

    Jangan digunakan untuk deployment final.
  */
  net.setInsecure();
#endif

  // ==========================================================
  // MQTT
  // ==========================================================
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);

  Serial.println("[SYSTEM] setup selesai");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // ==========================================================
  // WIFI
  // ==========================================================
  connectWiFi();

  // ==========================================================
  // MQTT
  // ==========================================================
  connectMqtt();

  if (mqtt.connected()) {
    mqtt.loop();
    flushEvents();
  }

  // ==========================================================
  // TARE COMMAND
  // ==========================================================
  if (pendingTare) {
    pendingTare = false;
    Serial.println("[CMD] melakukan tare...");

    if (scale.is_ready()) {
      scale.tare();
      Serial.println("[CMD] tare selesai");
      emitEvent("tare", "OK", 0, 0, "manual_tare", "manual");
    } else {
      Serial.println("[CMD] tare gagal: HX711 tidak siap");
      emitEvent("tare", "FAILED", 0, 0, "hx711_not_ready", "manual");
    }
  }

  // ==========================================================
  // MANUAL FEED COMMAND
  // ==========================================================
  if (pendingFeed) {
    pendingFeed = false;
    feedPortion(pendingFeedG, "manual");
  }

  // ==========================================================
  // SCHEDULE
  // ==========================================================
  runScheduler();

  // ==========================================================
  // SENSOR SAMPLING
  // ==========================================================
  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = millis();
    sampleAndPublish();
  }

  // ==========================================================
  // SMALL DELAY
  // ==========================================================
  delay(5);
}
