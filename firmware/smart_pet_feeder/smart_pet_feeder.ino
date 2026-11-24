/*
  Smart Pet Feeder - firmware ESP32 (versi awal / placeholder yang sudah jalan di Wokwi)

  Alur: sensor -> logika EDGE -> MQTT over TLS (8883) -> cloud/dashboard
  Edge (tetap jalan walau internet putus):
    1. Jadwal makan lokal (NTP sekali sync, lalu RTC ESP32 jalan sendiri)
    2. Cek stok (HC-SR04) + deteksi low_stock dengan histeresis
    3. Ukur porsi (load cell) -> status feeding OK / FAILED
    4. Batas harian & jeda minimum (anti overfeeding)
    5. Report-by-exception: kirim data hanya bila berubah / heartbeat
    6. Event dibuffer dan dikirim ulang saat koneksi pulih

  Library: PubSubClient, ArduinoJson (v7), ESP32Servo, HX711 (bogde)
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "ca_cert.h"

WiFiClientSecure net;
PubSubClient mqtt(net);
HX711 scale;
Servo gate;

char topicTelemetry[64], topicEvent[64], topicStatus[64], topicCmd[64];

// ---------- state ----------
static float    lastPubStock = -100, lastPubWeight = -1000;
static uint32_t lastPubMs = 0, lastSampleMs = 0, lastMqttTry = 0, lastFeedMs = 0;
static uint32_t samplesTaken = 0, msgsSent = 0;
static float    dailyGrams = 0;
static int      lastYday = -1, lastFeedKey = -1;
static bool     lowStockFlag = false;
static volatile bool pendingFeed = false, pendingTare = false;
static float    pendingFeedG = DEFAULT_PORTION_G;

// ---------- buffer event (ring buffer) ----------
#define EVT_BUF 10
#define EVT_LEN 220
static char    evtBuf[EVT_BUF][EVT_LEN];
static uint8_t evtHead = 0, evtCount = 0;

static void queueEvent(const char* json) {
  if (evtCount == EVT_BUF) { evtHead = (evtHead + 1) % EVT_BUF; evtCount--; }  // buang yang terlama
  strlcpy(evtBuf[(evtHead + evtCount) % EVT_BUF], json, EVT_LEN);
  evtCount++;
}

static void flushEvents() {
  while (evtCount && mqtt.connected()) {
    if (!mqtt.publish(topicEvent, evtBuf[evtHead], false)) break;
    evtHead = (evtHead + 1) % EVT_BUF;
    evtCount--;
    msgsSent++;
  }
}

static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1600000000 ? (uint32_t)t : 0;  // 0 = waktu belum tersinkron
}

static void emitEvent(const char* type, const char* status, float targetG, float deliveredG, const char* reason) {
  JsonDocument doc;
  doc["ts"] = nowEpoch();
  doc["device"] = DEVICE_ID;
  doc["type"] = type;
  doc["status"] = status;
  doc["target_g"] = targetG;
  doc["delivered_g"] = deliveredG;
  doc["reason"] = reason;
  char buf[EVT_LEN];
  serializeJson(doc, buf, sizeof(buf));
  Serial.printf("[EVENT] %s\n", buf);
  queueEvent(buf);
}

// ---------- sensor ----------
static float readDistanceCm() {
  float valid[3]; int n = 0;
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long us = pulseIn(PIN_ECHO, HIGH, 30000);
    if (us > 0) valid[n++] = us * 0.0343f / 2.0f;
    delay(30);
  }
  if (n == 0) return NAN;
  for (int i = 1; i < n; i++) {            // insertion sort, ambil median
    float k = valid[i]; int j = i - 1;
    while (j >= 0 && valid[j] > k) { valid[j + 1] = valid[j]; j--; }
    valid[j + 1] = k;
  }
  return valid[n / 2];
}

static float stockPercent(float cm) {
  if (isnan(cm)) return -1;
  float pct = (BOWL_EMPTY_CM - cm) / (BOWL_EMPTY_CM - BOWL_FULL_CM) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

static float readWeightG() {
  if (!scale.is_ready()) return NAN;
  return scale.get_units(3);
}

// ---------- aktuator: feeding ----------
static void feedPortion(float targetG, const char* trigger) {
  uint32_t now = millis();
  if (lastFeedMs && now - lastFeedMs < MIN_FEED_GAP_MS) {
    emitEvent("feed", "REJECTED", targetG, 0, "too_soon"); return;
  }
  if (dailyGrams + targetG > MAX_DAILY_GRAMS) {
    emitEvent("feed", "REJECTED", targetG, 0, "daily_limit"); return;
  }
  Serial.printf("[FEED] mulai, target %.0f g (%s)\n", targetG, trigger);
  float start = readWeightG(); if (isnan(start)) start = 0;
  gate.write(SERVO_OPEN_DEG);
  uint32_t t0 = millis();
  float delivered = 0;
  while (millis() - t0 < FEED_TIMEOUT_MS) {
    delay(100);
    float w = readWeightG();
    delivered = isnan(w) ? 0 : w - start;
    if (delivered >= targetG) break;
  }
  gate.write(SERVO_CLOSED_DEG);
  lastFeedMs = millis();
  bool ok = delivered >= targetG * MIN_RATIO_OK;
  if (ok) dailyGrams += delivered;
  emitEvent("feed", ok ? "OK" : "FAILED", targetG, delivered, ok ? trigger : "under_delivered_or_jam");
}

static void runScheduler() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;                    // waktu belum sync -> lewati
  if (t.tm_yday != lastYday) { lastYday = t.tm_yday; dailyGrams = 0; }
  for (uint8_t h : FEED_HOURS) {
    int key = t.tm_yday * 24 + h;
    if (t.tm_hour == h && t.tm_min == 0 && key != lastFeedKey) {
      lastFeedKey = key;
      feedPortion(DEFAULT_PORTION_G, "schedule");
    }
  }
}

// ---------- MQTT ----------
static void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* action = doc["action"] | "";
  if (!strcmp(action, "feed")) {
    float g = doc["grams"] | (float)DEFAULT_PORTION_G;
    pendingFeedG = constrain(g, 5.0f, 60.0f);
    pendingFeed = true;
  } else if (!strcmp(action, "tare")) {
    pendingTare = true;
  }
}

static void connectMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < 5000) return;             // non-blocking, coba tiap 5 dtk
  lastMqttTry = millis();
  String cid = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] connecting... ");
  if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, topicStatus, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(topicStatus, "online", true);
    mqtt.subscribe(topicCmd, 1);
  } else {
    Serial.printf("gagal rc=%d\n", mqtt.state());
  }
}

static void sampleAndPublish() {
  samplesTaken++;
  float cm = readDistanceCm();
  float stock = stockPercent(cm);
  float w = readWeightG();

  // keputusan edge: low stock dengan histeresis (tidak spam)
  if (stock >= 0) {
    if (!lowStockFlag && stock < LOW_STOCK_PCT) { lowStockFlag = true;  emitEvent("low_stock", "ALERT", 0, 0, "stock_below_threshold"); }
    if (lowStockFlag && stock > LOW_STOCK_PCT + 10) lowStockFlag = false;
  }

  // keputusan edge: kirim hanya bila ada perubahan berarti / heartbeat
  bool changed = fabsf(stock - lastPubStock) >= DELTA_STOCK_PCT ||
                 (!isnan(w) && fabsf(w - lastPubWeight) >= DELTA_WEIGHT_G);
  bool beat = millis() - lastPubMs >= HEARTBEAT_MS;
  if (!(changed || beat || lastPubMs == 0)) return;

  JsonDocument doc;
  doc["ts"] = nowEpoch();
  doc["device"] = DEVICE_ID;
  doc["distance_cm"] = isnan(cm) ? -1 : cm;
  doc["stock_pct"] = stock;
  doc["bowl_g"] = isnan(w) ? -1 : w;
  doc["daily_g"] = dailyGrams;
  doc["rssi"] = WiFi.RSSI();
  doc["samples"] = samplesTaken;   // bahan tabel perbandingan sebelum/sesudah edge
  doc["sent"] = msgsSent + 1;
  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  Serial.printf("[TELEMETRY] %s\n", buf);
  if (mqtt.connected() && mqtt.publish(topicTelemetry, buf, false)) {
    msgsSent++;
    lastPubMs = millis(); lastPubStock = stock; if (!isnan(w)) lastPubWeight = w;
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);

  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(HX711_CAL_FACTOR);
  scale.tare();

  gate.setPeriodHertz(50);
  gate.attach(PIN_SERVO, 500, 2400);
  gate.write(SERVO_CLOSED_DEG);

  snprintf(topicTelemetry, sizeof(topicTelemetry), "petfeeder/%s/telemetry", DEVICE_ID);
  snprintf(topicEvent,     sizeof(topicEvent),     "petfeeder/%s/event",     DEVICE_ID);
  snprintf(topicStatus,    sizeof(topicStatus),    "petfeeder/%s/status",    DEVICE_ID);
  snprintf(topicCmd,       sizeof(topicCmd),       "petfeeder/%s/cmd",       DEVICE_ID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\n[WiFi] terhubung" : "\n[WiFi] belum terhubung (mode offline, edge tetap jalan)");

  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");  // sync sekali; RTC lanjut sendiri

#if TLS_VERIFY_CA
  net.setCACert(ROOT_CA);       // verifikasi sertifikat broker (butuh waktu valid dari NTP)
#else
  net.setInsecure();            // HANYA untuk simulasi. Final: TLS_VERIFY_CA = 1
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);
}

void loop() {
  connectMqtt();
  mqtt.loop();
  flushEvents();

  if (pendingTare) { pendingTare = false; scale.tare(); Serial.println("[CMD] tare"); }
  if (pendingFeed) { pendingFeed = false; feedPortion(pendingFeedG, "manual"); }

  runScheduler();

  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = millis();
    sampleAndPublish();
  }
}
