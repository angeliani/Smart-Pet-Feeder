# Langkah Wokwi
1. Buka wokwi.com, New Project, pilih ESP32, Arduino.
2. Tab `diagram.json`: tempel isi `wokwi/diagram.json`.
3. Tab `libraries.txt`: tempel isi `wokwi/libraries.txt`.
4. Tab `sketch.ino`: tempel isi `firmware/smart_pet_feeder/smart_pet_feeder.ino`.
5. Buat file baru (ikon panah di sebelah tab): `config.h`, `secrets.h`, `ca_cert.h`, tempel isinya dari repo (`secrets.h` dari `secrets.h.example`, WiFi `Wokwi-GUEST`).
6. Isi `MQTT_HOST/USER/PASS` dengan broker TLS (mis. HiveMQ Cloud free, port 8883).
7. Klik Start. Cek Serial Monitor.
8. Uji: ubah jarak HC-SR04 (stok), geser berat load cell, kirim `{"action":"feed"}` ke topik cmd lewat MQTT client.
